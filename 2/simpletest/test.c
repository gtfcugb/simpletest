#include <assert.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/sem.h>

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <curl/curl.h>  

#define MAX_THREAD_NUM 1024
#define IP_LEN  32
#define CMD_NUM 10

enum Cmd{
    CMD_SET     =   1,
    CMD_GET     =   2,
    CMD_CODE    =   3,
        CMD_RAND=   6
};

struct RunDataSt
{
    int             appId;
    int             index;      
    int             joinStatus;
    int             cmd;        /*线程执行命令*/
    int             loop;       /*线程执行命令次数*/
    int             okTimes;    /*成功次数*/
    int             failTimes;  /*失败次数*/

    pthread_t       tid;        /*线程id*/
    struct timeval  tvStart;    /*开始时间*/
        struct timeval  tvEnd;      /*结束时间*/
};

typedef struct RunDataSt RunData;

struct ServerDataSt
{
    int threadNum;
    int loop;
    int cmd;
    int port;
    char ip[IP_LEN];
    int print;
    char appId[64];
    int daemon;
    int preLen;
    char*preValue;
    int sleep;
};
typedef struct ServerDataSt ServerData;



RunData gRunData[MAX_THREAD_NUM];
ServerData gServerData;
int gExit=0;

#define FLOG_DEBUG(format,...) if (gServerData.print == 1)  {printf(format,## __VA_ARGS__);}
#define FLOG_NOTICE(format,...) if (1)  {printf(format,## __VA_ARGS__);syslog(LOG_NOTICE,format,## __VA_ARGS__);}

static char sCmdDesc[CMD_NUM][32]={
    "",
    "set",
    "get",
    "code",
};

static void showHelp(){
    printf("-n (必选) \t: 开启线程数量\n");
    printf("-c (必选) \t: 执行测试命令\n\t\t\t1 set操作\n\t\t\t2 get操作\n");
    printf("-h (必选) \t: 服务端ip\n");
    printf("-p (必选) \t: 服务端端口\n");
    printf("-y (可选) \t: 设置值的（前缀）长度，默认为16 ,最大不超过10000 \n");
    printf("-t (可选) \t: 编译时间\n");
    printf("-l (可选) \t: 每个线程运行次数\n");
    printf("-d (可选) \t: 后台运行\n");
    printf("-print (可选) \t: 打印日志（调试使用）\n");
    printf("-help (可选) \t: 帮助\n");
}


/** 
*@brief 消息处理函数，回调
*/
static void destroySignal(int sig){
    if(SIGINT == sig){
        gExit = 1;
    }
    gExit = 1;
}

int utilSignalInit(){
	/*
	 * 为了防止SIGPIPEsignal而引起的强制结束服务器运行的问题
	 */
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_handler = SIG_IGN;
	act.sa_flags = 0;
	act.sa_flags &= ~SA_RESETHAND;
	sigaction(SIGPIPE, &act, NULL);

	/*
	 * 当发生强制结束服务器运行的signal时，调用正常结束服务器的运行函数
	 */
	//signal(SIGSEGV, segError);
	signal(SIGINT, 	destroySignal);
	signal(SIGKILL, destroySignal);
	signal(SIGQUIT, destroySignal);
	signal(SIGTERM, destroySignal);
	return 0;
}

int utilDaemonize(const char* cmd,void(*callBack)()){
	char msg[1024];
    int i,fd0,fd1,fd2;
	pid_t pid;
	struct rlimit rl;
	struct sigaction sa;
	umask(0);
	if( getrlimit(RLIMIT_NOFILE,&rl) < 0){
		sprintf(msg,"%s : can't get file limit",cmd);
		goto ERROR_OUT;
	}
	if((pid = fork() )< 0){
		sprintf(msg,"%s: can not fork",cmd);
		goto ERROR_OUT;
	}
	else if(pid != 0){
		callBack();
		exit(0);
	}	
	setsid();
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if( sigaction(SIGHUP,&sa,NULL) < 0){
		sprintf(msg,"%s: can not ignore SIGHUP",cmd);
		goto ERROR_OUT;
	}
	if((pid = fork()) < 0){
		sprintf(msg,"%s: can not fork",cmd);
		goto ERROR_OUT;
	}
	else if(pid == 0){
		callBack();
		exit(0);
	}		
	if(chdir("/var/run/") < 0){
		sprintf(msg,"%s: can NOt change directory to /",cmd);
		goto ERROR_OUT;
	}	
	if(rl.rlim_max == RLIM_INFINITY)
		rl.rlim_max = 1024;
	rl.rlim_max = 3;
	for(i = 0 ; i < rl.rlim_max ; i++){
		close(i);
	}
	fd0 = open("/dev/null",O_RDWR);
	fd1 = dup(0);
	fd2 = dup(0);
	//int logmask;
	//openlog(cmd,LOG_CONS,LOG_LOCAL1);
	//logmask = setlogmask(LOG_UPTO(LOG_NOTICE));
    openlog("tycache_test",LOG_CONS | LOG_PID,LOG_USER);

	if( fd0 != 0 || fd1 != 1||fd2!= 2){
		sprintf(msg,"unexpected file descriptor %d %d %d",fd0,fd1,fd2);
		goto ERROR_OUT;
	}
	FLOG_DEBUG("%s %s",cmd,"成功切换为守护进程\n");
	return 0;
ERROR_OUT:
	FLOG_DEBUG("%s",msg);
	return -1;
}
void daemCallBack(){
    FLOG_DEBUG("daemCallBack\n");
}

int utilArgs(int argc,char** argv){
    gServerData.print       =   0;
    gServerData.loop        =   0;
    gServerData.daemon      =   0;
    gServerData.preLen      =   16;
    gServerData.sleep       =   0;
    int i=0;
    for (i = 1; i < argc; i++) {
        if (strcmp("-c", argv[i]) == 0) {
            gServerData.cmd =    atoi(argv[i + 1]);
        }
        if (strcmp("-d", argv[i]) == 0) {
            gServerData.daemon =    1;
        }
        if(strncmp(argv[i],"-n",2 ) == 0) {
            gServerData.threadNum =    atoi(argv[i + 1]);
        }
        if(strncmp(argv[i],"-l",2 ) == 0) {
            gServerData.loop        =    atoi(argv[i + 1]);
        }
        if(strncmp(argv[i],"-y",2 ) == 0) {
            gServerData.preLen      =    atoi(argv[i + 1]);
        }
        if(strncmp(argv[i],"-s",2 ) == 0) {
            gServerData.sleep       =    atoi(argv[i + 1]);
        }
        if(strncmp(argv[i],"-print",6 ) == 0) {
            gServerData.print       =    1;
        }
        else if(strncmp(argv[i],"-p",2 ) == 0) {
            gServerData.port       =    atoi(argv[i + 1]);
        }
        
        if(strncmp(argv[i],"-help",5 ) == 0) {
            goto UTIL_HELP;
        }
        if(strncmp(argv[i],"-h",2 ) == 0) {
            if(strlen(argv[i + 1]) >= IP_LEN -1){
                goto UTIL_HELP;
            }
            strcpy(gServerData.ip,argv[i + 1]);
        }
        if(strncmp(argv[i],"-t",2 ) == 0) {
            printf("COMPILE TIME : %s %s \n",__DATE__, __TIME__);
            exit(0);
        } 
    }
    if(gServerData.cmd == 0 || gServerData.threadNum == 0 || gServerData.loop == 0 || gServerData.port == 0){
        goto UTIL_HELP;
    }
    gServerData.preValue                        =   (char*)malloc(gServerData.preLen+1);
    memset(gServerData.preValue,'a',gServerData.preLen);
    gServerData.preValue[gServerData.preLen]    =   '\0';
    return 0;
UTIL_HELP:
    showHelp();
    exit(0);
    return -1;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)   
{   
    //printf("write callback %d \n",size*nmemb);

    return size*nmemb;  
  
}  

void *processGo(void*arg){
    RunData*pRunData    =   (RunData*)arg;
    FLOG_NOTICE("proccessGo index %d \n",pRunData->index);

            CURL *curl; 
	CURLcode resCode;
            curl=curl_easy_init();  

    gettimeofday(&pRunData->tvStart,0);

    int i;
    for( i =0 ;i < pRunData->loop;i++){
        if(gExit){
            break;
        }
        int    cmd =   pRunData->cmd;
        /*随机指派任务*/
        if(cmd  ==  CMD_RAND){
            cmd = rand()%4+1;
        }
        
        char proto[10240];
        if(cmd    ==  CMD_GET){
            char url[256];
            sprintf(url,"http://%s:%d/get?rand=%d",gServerData.ip,gServerData.port,i);

            curl_easy_setopt(curl, CURLOPT_URL,url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);   
            resCode	= curl_easy_perform(curl);  
	        assert(resCode == CURLE_OK );
 	        long infocode;
	        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&infocode);
            if(infocode != 200){
                printf("infocde %l \n",infocode);
            }
        }
        else if(cmd    ==  CMD_CODE){
            char url[256];
            sprintf(url,"http://%s:%d/code?rand=%d",gServerData.ip,gServerData.port,i);

            curl_easy_setopt(curl, CURLOPT_URL,url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);   
            resCode	= curl_easy_perform(curl);  
            assert(resCode == CURLE_OK );
 	        long infocode;
	        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&infocode);
            if(infocode != 200){
                printf("infocde %l \n",infocode);
            }
        }
        
        else{
            assert(0);
        }

            if(gServerData.print ){
            }

            if(gServerData.print ){
            }
            
            pRunData->okTimes++;

        if(gServerData.sleep){
            sleep(gServerData.sleep);
        }
        if(pRunData->cmd  ==  CMD_RAND){
            if(i >= pRunData->loop - 1){
                i = 0;
                continue;
            }
        }
    }
    curl_easy_cleanup(curl);
    gettimeofday(&pRunData->tvEnd,0);
    return NULL;
}

int main(int argc,char** argv){
    assert(utilArgs(argc,argv) == 0);
    /*check daemon run */
    if(gServerData.daemon == 1){
        utilDaemonize("/var/run/",daemCallBack);
        utilSignalInit();
    }
    curl_global_init(CURL_GLOBAL_ALL);

    int i   =   0;
    for( i =0;i<gServerData.threadNum;i++){
        gRunData[i].index   =   i;
        gRunData[i].loop    =   gServerData.loop;
        gRunData[i].cmd     =   gServerData.cmd;

        pthread_create(&(gRunData[i].tid), NULL, processGo, &gRunData[i]);
    }
    int minCost =   1000000000;
    int maxCost =   0;
    for( i =0;i<gServerData.threadNum;i++){
        pthread_join(gRunData[i].tid, NULL);
        int cost    =   (gRunData[i].tvEnd.tv_sec-gRunData[i].tvStart.tv_sec) * 1000000 + (gRunData[i].tvEnd.tv_usec - gRunData[i].tvStart.tv_usec);
        minCost     =   cost < minCost ? cost:minCost;
        maxCost     =   cost < maxCost ? maxCost:cost;

        FLOG_NOTICE("thread: %d \t cmd:%s \t loop:%d \t cost:%d \t okTimes: %d \t failTimes:%d \n",gRunData[i].index,sCmdDesc[gRunData[i].cmd],gRunData[i].okTimes+gRunData[i].failTimes,cost,gRunData[i].okTimes,gRunData[i].failTimes);
        
        if(gRunData[gServerData.threadNum].tvStart.tv_sec == 0){
            gRunData[gServerData.threadNum].tvStart.tv_sec      =   gRunData[i].tvStart.tv_sec;
            gRunData[gServerData.threadNum].tvStart.tv_usec     =   gRunData[i].tvStart.tv_usec;
            gRunData[gServerData.threadNum].tvEnd.tv_sec        =   gRunData[i].tvEnd.tv_sec;
            gRunData[gServerData.threadNum].tvEnd.tv_usec       =   gRunData[i].tvEnd.tv_usec;

        }
        else{
            if(gRunData[i].tvStart.tv_sec < gRunData[gServerData.threadNum].tvStart.tv_sec || (gRunData[i].tvStart.tv_sec == gRunData[gServerData.threadNum].tvStart.tv_sec && gRunData[i].tvStart.tv_usec < gRunData[gServerData.threadNum].tvStart.tv_usec)){
                gRunData[gServerData.threadNum].tvStart.tv_sec      =   gRunData[i].tvStart.tv_sec;
                gRunData[gServerData.threadNum].tvStart.tv_usec     =   gRunData[i].tvStart.tv_usec;
            }

            if(gRunData[i].tvEnd.tv_sec > gRunData[gServerData.threadNum].tvEnd.tv_sec || (gRunData[i].tvEnd.tv_sec == gRunData[gServerData.threadNum].tvEnd.tv_sec && gRunData[i].tvEnd.tv_usec > gRunData[gServerData.threadNum].tvEnd.tv_usec)){
                gRunData[gServerData.threadNum].tvEnd.tv_sec      =   gRunData[i].tvEnd.tv_sec;
                gRunData[gServerData.threadNum].tvEnd.tv_usec     =   gRunData[i].tvEnd.tv_usec;
            }
        }
        gRunData[gServerData.threadNum].okTimes     += gRunData[i].okTimes;
        gRunData[gServerData.threadNum].failTimes   += gRunData[i].failTimes;
        gRunData[gServerData.threadNum].loop        += gRunData[i].okTimes+gRunData[i].failTimes;
        gRunData[gServerData.threadNum].cmd         =  gRunData[i].cmd;
    }
    int cost    =   (gRunData[i].tvEnd.tv_sec-gRunData[i].tvStart.tv_sec) * 1000000 + (gRunData[i].tvEnd.tv_usec - gRunData[i].tvStart.tv_usec);


    FLOG_NOTICE("thread: all \t cmd:%s \t loop:%d \t mincost:%d\t maxcost:%d \t  cost:%d \t okTimes: %d \t failTimes:%d \t qps: %f \n",sCmdDesc[gRunData[i].cmd],gRunData[i].loop,minCost,maxCost,cost,gRunData[i].okTimes,gRunData[i].failTimes, (float)(gRunData[i].loop/((float)cost/1000000)));

    /*output test result */

    return 0;
}
