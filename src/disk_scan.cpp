#include "disk_scan.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <thread>
#include <cerrno>
#ifdef __FreeBSD__
#include <sys/disk.h>
#endif
#ifndef DIOCGSECTORSIZE
#define DIOCGSECTORSIZE 0x40046480UL
#endif
#ifndef DIOCGMEDIASIZE
#define DIOCGMEDIASIZE 0x40086481UL
#endif

static bool disk_candidate(const char* n) {
    return strncmp(n,"da",2)==0 || strncmp(n,"ada",3)==0 ||
           strncmp(n,"nvme",4)==0;
}
std::vector<DiskInfo> discover_disks() {
    std::vector<DiskInfo> out;
    DIR* d=opendir("/dev"); if(!d) return out;
    struct dirent* e;
    while((e=readdir(d))) {
        if(!disk_candidate(e->d_name)) continue;
        // Only whole-disk base names; skip common partition/slice suffixes.
        bool digits=false, valid=true;
        const char* p=e->d_name;
        if(strncmp(p,"nvme",4)==0) {
            p += 4; while(*p && (*p>='0'&&*p<='9')) {digits=true; ++p;}
            if(strncmp(p,"ns",2)==0) { p+=2; while(*p && (*p>='0'&&*p<='9')) ++p; }
            if(*p) valid=false;
        } else {
            while(*p && !(*p>='0'&&*p<='9')) ++p;
            while(*p>='0'&&*p<='9') {digits=true; ++p;}
            if(*p) valid=false;
        }
        if(!digits || !valid) continue;
        DiskInfo info; info.path=std::string("/dev/")+e->d_name; info.name=e->d_name;
        int fd=open(info.path.c_str(),O_RDONLY|O_NONBLOCK);
        if(fd<0) continue;
        struct stat st;
        if(fstat(fd,&st)==0 && S_ISREG(st.st_mode)) info.bytes=(uint64_t)st.st_size;
#ifdef DIOCGMEDIASIZE
        off_t media=0;
        if(ioctl(fd,DIOCGMEDIASIZE,&media)==0 && media>0) info.bytes=(uint64_t)media;
#endif
#ifdef DIOCGSECTORSIZE
        unsigned int ss=512;
        if(ioctl(fd,DIOCGSECTORSIZE,&ss)==0 && ss>0) info.sector_size=ss;
#endif
        close(fd);
        out.push_back(info);
    }
    closedir(d);
    std::sort(out.begin(),out.end(),[](const DiskInfo&a,const DiskInfo&b){return a.path<b.path;});
    return out;
}
const char* state_name(ScanState s) {
    switch(s){case ScanState::Idle:return "idle";case ScanState::Benchmark:return "benchmark";
    case ScanState::Running:return "running";case ScanState::Paused:return "paused";
    case ScanState::Finished:return "finished";case ScanState::Error:return "error";
    case ScanState::Stopping:return "stopping";} return "unknown";
}
bool start_scan(AppState& st,const std::string& path) {
    bool expected=false;
    if(!st.worker_active.compare_exchange_strong(expected,true)) return false;
    st.stop_requested=false; st.pause_requested=false;
    {
        std::lock_guard<std::mutex> l(st.mutex);
        st.scan_state=ScanState::Benchmark; st.device=path; st.processed_bytes=0;
        st.read_errors=0; st.current_lba=0; st.speed_bps=0; st.eta_seconds=-1;
        st.message="Предварительный замер скорости";
        st.block_map.assign(500,0);
    }
    std::thread([&st,path](){
        int fd=open(path.c_str(),O_RDONLY);
        if(fd<0){std::lock_guard<std::mutex> l(st.mutex);st.scan_state=ScanState::Error;st.message="Не удалось открыть диск";st.worker_active=false;return;}
        uint64_t size=0; uint32_t sector=512;
#ifdef DIOCGMEDIASIZE
        off_t media=0; if(ioctl(fd,DIOCGMEDIASIZE,&media)==0 && media>0) size=(uint64_t)media;
#endif
#ifdef DIOCGSECTORSIZE
        unsigned int ss=512; if(ioctl(fd,DIOCGSECTORSIZE,&ss)==0 && ss>0) sector=ss;
#endif
        if(size==0){struct stat sb;if(fstat(fd,&sb)==0&&sb.st_size>0)size=(uint64_t)sb.st_size;}
        if(size==0){close(fd);std::lock_guard<std::mutex> l(st.mutex);st.scan_state=ScanState::Error;st.message="Не удалось определить размер диска";st.worker_active=false;return;}
        const size_t chunk=1024*1024;
        std::vector<unsigned char> buf(chunk);
        // Quick read-only benchmark at three disk locations before the full scan.
        double bench_sum=0.0; int bench_ok=0;
        const uint64_t sample=(size < 16ULL*1024*1024) ? size : 16ULL*1024*1024;
        uint64_t offsets[3]={0, (size>sample?(size-sample)/2:0), (size>sample?size-sample:0)};
        for(int bi=0;bi<3 && !st.stop_requested.load();++bi){
            uint64_t done=0; auto bt=std::chrono::steady_clock::now();
            while(done<sample && !st.stop_requested.load()){
                size_t want=(sample-done>buf.size())?buf.size():(size_t)(sample-done);
                ssize_t n=pread(fd,buf.data(),want,(off_t)(offsets[bi]+done));
                if(n<=0) break;
                done+=(uint64_t)n;
            }
            double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-bt).count();
            if(done>0 && sec>0){bench_sum+=(double)done/sec;++bench_ok;}
        }
        double bench_speed=bench_ok?bench_sum/bench_ok:0.0;
        if(st.stop_requested.load()){
            close(fd); std::lock_guard<std::mutex> l(st.mutex);
            st.scan_state=ScanState::Idle;st.message="Сканирование остановлено";st.worker_active=false;return;
        }
        auto t0=std::chrono::steady_clock::now(), last=t0;
        uint64_t pos=0, errors=0; double speed=bench_speed;
        {
            std::lock_guard<std::mutex> l(st.mutex);
            st.total_bytes=size;st.sector_size=sector;st.scan_state=ScanState::Running;
            st.speed_bps=bench_speed;st.eta_seconds=(bench_speed>1)?(int64_t)(size/bench_speed):-1;
            st.message=bench_ok?"Замер завершён; начинается полное сканирование":"Замер скорости не удался; начинается сканирование";
        }
        while(pos<size && !st.stop_requested.load()) {
            while(st.pause_requested.load()&&!st.stop_requested.load()) {
                {std::lock_guard<std::mutex> l(st.mutex);st.scan_state=ScanState::Paused;st.message="Сканирование приостановлено";}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(st.stop_requested.load()) break;
            size_t want=(size-pos>chunk)?chunk:(size_t)(size-pos);
            ssize_t n=pread(fd,buf.data(),want,(off_t)pos);
            if(n<=0){++errors; {std::lock_guard<std::mutex> l(st.mutex);size_t bi=(size?((long double)pos/size*st.block_map.size()):0);if(bi<st.block_map.size())st.block_map[bi]=2;}}
            else {size_t bi=(size?((long double)pos/size*st.block_map.size()):0);std::lock_guard<std::mutex> l(st.mutex);if(bi<st.block_map.size())st.block_map[bi]=1;}
            pos += want;
            auto now=std::chrono::steady_clock::now();
            double elapsed=std::chrono::duration<double>(now-t0).count();
            double delta=std::chrono::duration<double>(now-last).count();
            if(delta>0) speed=(double)pos/elapsed;
            last=now;
            {
                std::lock_guard<std::mutex> l(st.mutex);
                st.processed_bytes=pos;st.read_errors=errors;st.current_lba=pos/sector;st.speed_bps=speed;
                st.eta_seconds=(speed>1)?(int64_t)((size-pos)/speed):-1;
                st.scan_state=ScanState::Running;
                st.message=errors?"Сканирование: обнаружены ошибки чтения":"Сканирование поверхности (только чтение)";
            }
        }
        close(fd);
        {std::lock_guard<std::mutex> l(st.mutex);
        st.processed_bytes=pos;st.read_errors=errors;st.speed_bps=speed;st.eta_seconds=-1;
        st.scan_state=st.stop_requested?ScanState::Idle:(errors?ScanState::Error:ScanState::Finished);
        st.message=st.stop_requested?"Сканирование остановлено":(errors?"Завершено: есть ошибки чтения":"Сканирование завершено");}
        st.worker_active=false;
    }).detach();
    return true;
}
void pause_scan(AppState& s){if(s.worker_active)s.pause_requested=true;}
void resume_scan(AppState& s){s.pause_requested=false;}
void stop_scan(AppState& s){if(s.worker_active){s.stop_requested=true;std::lock_guard<std::mutex> l(s.mutex);s.scan_state=ScanState::Stopping;s.message="Остановка сканирования...";}}
