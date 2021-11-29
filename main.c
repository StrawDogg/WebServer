#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位，5秒

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];   //0是读端，1是写端
static sort_timer_lst timer_lst;    //定时器容器类的对象

static int epollfd = 0;

//信号处理函数,该信号来时候，系统调用将被中断，并执行该函数，
//该函数只是简单通知主循环，并把信号值传递给主循环
//执行步骤还是在主循环里，具体执行目标信号对应的逻辑
//该函数往管道写入端写入信号值，主循环则从管道的读端读取该信号
//只通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno（可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据）
    //每种错误都有一个错误码与之对应，查看对应的错误码就知道发生什么错误了
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);    //将信号值写入管道(char*型)，因为epoll一直在监听pipefd[0]读端，一有信号就会传递给主循环
    errno = save_errno;
}


//设置信号函数,告诉系统处理函数要处理哪些信号(SIGPIPE，SIGALRM，SIGTERM)，以及谁来处理(sig_handler()来处理)
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;    //指定信号处理函数
    if (restart)
        sa.sa_flags |= SA_RESTART;//设置程序收到信号时的行为
    sigfillset(&sa.sa_mask);    //sigdillset函数是用于将 信号集合初始化，sa.sa_mask是要屏蔽的信号
    // sigaction函数，sig参数是要捕获的信号类型，sa是新的信号处理方式
    assert(sigaction(sig, &sa, NULL) != -1);    //检查或修改 与指定信号相关联的处理动作
}

//定时处理任务，每隔 TIMESLOT = 5 秒给这个函数的进程发送一个SIGALRM信号
// 定时器模块的功能是定时检查长时间无反应的连接，如果有服务器这边就主动断开连接。
void timer_handler()
{
    timer_lst.tick();   //tick()才是真正的处理定时器处理函数
    alarm(TIMESLOT);    //alarm(5)表示5秒之后给程序发送一个 SIGALRM 信号，接到SIGALRM后又会调用这个函数，形成循环

    // 信号处理函数利用管道通知主循环，主循环接收到信号后，会对升序链表上所有定时器进行处理，
    // 若该段时间内没有交换数据，则关闭连接释放资源
}

//定时器回调函数(信号处理函数)，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);    // 删除注册在epoll上的非活动连接socket
    assert(user_data);  
    close(user_data->sockfd);      //关闭文件描述符
    http_conn::m_user_count--;      //连接数减一
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}
 
void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
    printf("start!!\n");
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif
    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    //往一个读端关闭的管道或socket连接中写数据时，将引发SIGPIPE信号。
    //需要捕获它并处理，至少也得忽略它。因为程序收到SIGPIPE信号会默认结束该进程
    //我们不希望应为错误的写操作导致程序退出
    addsig(SIGPIPE, SIG_IGN);   //SIG_IGN表示信号处理函数为忽略处理


    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    printf("init\n");
    connPool->init("127.0.0.1", "root", "746f7657465952fd", "yourdb", 3306, 8);
    printf("database connected! \n");

    //创建线程池，里面的线程一直在while(1)死循环，阻塞等待信号量大于0，就代表任务来了把信号量先减1然后去执行任务(且这个操作加了锁，保证只有一个线程去干)
    //线程池可以避免线程的频繁创建和销毁。新建立连接时，将已连接的socket放入到一个队列里面，然后线程池的线程负责从队列中取出来进行处理
    //队列是全局的，每个线程都会操作，为避免多线程竞争，线程在操作这个队列前要加锁
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];   // http对象，一开始就创建65536个？
    assert(users);


    printf("threadpool create! \n");
    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    //往epoll内核时间表中注册socket，当listen到新连接时，
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);  //设置管道写端为非阻塞，阻塞(先读/写的话，必须等到有东西(另一端写/读)否则阻塞)
    addfd(epollfd, pipefd[0], false);   // 管道读端加入epoll,开始监听

    //设置信号处理的函数，只处理alarm和terminal，（唤醒和终止）两种情况
    //这里指定信号处理函数为自定的信号处理函数sig_handler，有这两种信号来得时候，会中断调用sig_handler
    addsig(SIGALRM, sig_handler, false);    //由alarm或setitimer设置的时钟超时引起的
    addsig(SIGTERM, sig_handler, false);    //终止进程，kill命令发送的就是SIGTERM
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    
    //开始每隔5秒进行超时检测
    alarm(TIMESLOT);

    while (!stop_server)
    {
        //把socket放到epoll的红黑树之外，还会把就绪事件放在一个双向链表中，
        //每次调用epoll_wait，会把这个链表清空，但是后续LT和ET的处理方式不同
        //LT模式下，只要事件没处理完，调用epoll_wait时还会记录这个事件(原理是调用epoll_wait后，还会把这个socket放回到链表中)
        //ET模式仅在第一次返回(也就是说ET不会放回到被清空的链表中)
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); //如果没有就绪事件，epoll_wait会阻塞等待，然后返回事件的数目
        // events用来记录被触发的事件
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        for (int i = 0; i < number; i++)    
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接 
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address); //初始化socket地址(协议族，ip，端口号)，把事件注册到epoll上，然后初始化一堆数据

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0); //ret正常情况下都是1，即收到的信号长度是1
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }   
                else    //这里本应该会读到东西的，没读到说明出错了，直接删除这个事件(自己猜想)
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else    //同理，这里本应该可以写东西的，没写到说明出错了，直接删除这个事件(自己猜想)
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
