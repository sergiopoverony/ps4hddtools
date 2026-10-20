#include "http_server.h"
#include "disk_scan.h"
#include "index_html.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static std::string json_escape(const std::string& s){std::string o;for(unsigned char c:s){if(c=='"'||c=='\\')o+='\\';if(c>=32)o+=(char)c;}return o;}
static bool send_all(int fd,const std::string& s){size_t n=0;while(n<s.size()){ssize_t r=send(fd,s.data()+n,s.size()-n,0);if(r<=0)return false;n+=(size_t)r;}return true;}
static void reply(int fd,int code,const char* type,const std::string& body){const char* reason=code==200?"OK":code==400?"Bad Request":code==409?"Conflict":code==404?"Not Found":"Internal Server Error";std::ostringstream h;h<<"HTTP/1.1 "<<code<<" "<<reason<<"\r\nContent-Type: "<<type<<"\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: "<<body.size()<<"\r\n\r\n";send_all(fd,h.str());send_all(fd,body);}
static std::string status_json(AppState& s){std::lock_guard<std::mutex> l(s.mutex);std::ostringstream o;o<<"{\"state\":\""<<state_name(s.scan_state)<<"\",\"device\":\""<<json_escape(s.device)<<"\",\"total_bytes\":"<<s.total_bytes<<",\"processed_bytes\":"<<s.processed_bytes<<",\"percent\":"<<(s.total_bytes?100.0*(double)s.processed_bytes/s.total_bytes:0)<<",\"read_errors\":"<<s.read_errors<<",\"current_lba\":"<<s.current_lba<<",\"sector_size\":"<<s.sector_size<<",\"speed_bps\":"<<s.speed_bps<<",\"eta_seconds\":"<<s.eta_seconds<<",\"message\":\""<<json_escape(s.message)<<"\",\"block_map\":[";for(size_t i=0;i<s.block_map.size();++i){if(i)o<<',';o<<(int)s.block_map[i];}o<<"]}";return o.str();}
static std::string disks_json(AppState& s){std::vector<DiskInfo> ds=discover_disks();{std::lock_guard<std::mutex> l(s.mutex);s.disks=ds;}std::ostringstream o;o<<"{\"disks\":[";for(size_t i=0;i<ds.size();++i){if(i)o<<',';o<<"{\"path\":\""<<json_escape(ds[i].path)<<"\",\"name\":\""<<json_escape(ds[i].name)<<"\",\"bytes\":"<<ds[i].bytes<<",\"sector_size\":"<<ds[i].sector_size<<"}";}o<<"]}";return o.str();}
static std::string get_body(int fd,std::string req){size_t p=req.find("\r\n\r\n");if(p==std::string::npos)return "";std::string body=req.substr(p+4);size_t cl=0,cp=req.find("Content-Length:");if(cp==std::string::npos)cp=req.find("content-length:");if(cp!=std::string::npos){cp=req.find(':',cp)+1;while(cp<req.size()&&(req[cp]==' '||req[cp]=='\t'))++cp;while(cp<req.size()&&req[cp]>='0'&&req[cp]<='9'){cl=cl*10+(req[cp++]-'0');if(cl>65536)return "";}}while(body.size()<cl){char b[2048];size_t need=std::min(sizeof(b),cl-body.size());ssize_t n=recv(fd,b,need,0);if(n<=0)break;body.append(b,(size_t)n);}if(body.size()>cl&&cl)body.resize(cl);return body;}
static void handle_client(int c,AppState* state){
 char buf[8192];std::string req;size_t head_end=std::string::npos;
 while(req.size()<65536 && head_end==std::string::npos){ssize_t n=recv(c,buf,sizeof(buf),0);if(n<=0){close(c);return;}req.append(buf,(size_t)n);head_end=req.find("\r\n\r\n");}
 if(head_end==std::string::npos){reply(c,400,"application/json","{\"error\":\"bad request\"}");close(c);return;}
 std::istringstream is(req.substr(0,head_end));std::string method,path;is>>method>>path;
 // Ignore query strings so /?x=1 and /index.html are served like the root page.
 const size_t query_pos=path.find('?'); if(query_pos!=std::string::npos) path.resize(query_pos);
 if(method=="OPTIONS")reply(c,200,"text/plain","");
 else if(method=="GET"&&(path=="/"||path=="/index.html"))reply(c,200,"text/html; charset=utf-8",PS4HDDTOOLS_INDEX_HTML);
 else if(method=="GET"&&path=="/favicon.ico")reply(c,404,"text/plain","");
 else if(method=="GET"&&path=="/api/status")reply(c,200,"application/json",status_json(*state));
 else if(method=="GET"&&path=="/api/disks")reply(c,200,"application/json",disks_json(*state));
 else if(method=="POST"&&(path=="/api/scan/start"||path=="/api/scan/pause"||path=="/api/scan/resume"||path=="/api/scan/stop")){
   std::string body=get_body(c,req);
   if(path=="/api/scan/start") {auto p=body.find("\"device\"");if(p==std::string::npos){reply(c,400,"application/json","{\"error\":\"device required\"}");}else{auto colon=body.find(':',p),q1=body.find('"',colon),q2=body.find('"',q1+1);std::string dev=(q1!=std::string::npos&&q2!=std::string::npos)?body.substr(q1+1,q2-q1-1):"";if(dev.find("/dev/")!=0||dev.find("..")!=std::string::npos){reply(c,400,"application/json","{\"error\":\"invalid device\"}");}else if(!start_scan(*state,dev))reply(c,409,"application/json","{\"error\":\"scan already active\"}");else reply(c,200,"application/json","{\"ok\":true}");}}
   else if(path=="/api/scan/pause"){pause_scan(*state);reply(c,200,"application/json","{\"ok\":true}");}
   else if(path=="/api/scan/resume"){resume_scan(*state);reply(c,200,"application/json","{\"ok\":true}");}
   else {stop_scan(*state);reply(c,200,"application/json","{\"ok\":true}");}
 } else reply(c,404,"application/json","{\"error\":\"not found\"}");
 close(c);
}
int run_http_server(AppState& state,const char*,int port){int server=socket(AF_INET,SOCK_STREAM,0);if(server<0)return -1;int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons((uint16_t)port);a.sin_addr.s_addr=htonl(INADDR_ANY);if(bind(server,(sockaddr*)&a,sizeof(a))<0||listen(server,16)<0){close(server);return -2;}for(;;){int c=accept(server,nullptr,nullptr);if(c<0){if(errno==EINTR)continue;continue;}std::thread(handle_client,c,&state).detach();}}
