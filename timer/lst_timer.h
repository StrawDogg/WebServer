#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer;
struct client_data
{
    sockaddr_in address;    //地址族，端口号，ip
    int sockfd;
    util_timer *timer;
};


// 定时器类
class util_timer    // 将连接资源(client_data)、定时事件、和超时时间封装，双向链表
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;  // 超时时间 expire = time() + 3*5;
    void (*cb_func)(client_data *); //定时事件为回调函数，就是处理信号的函数，这里是删除非活动socket上的注册时间并关闭
    client_data *user_data; // 连接资源，包括这个定时器对应的socket地址，文件描述符，定时器
    util_timer *prev;
    util_timer *next;
};

// 定时器容器类，将定时器串联起来统一处理，成员变量只有head节点和tail节点
// 按照超时时间升序排列
class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    void add_timer(util_timer *timer)   //添加定时器
    {
        if (!timer)
        {
            return;
        }
        if (!head)  //没头结点的话表明第一次创建，所以把头和尾都赋为timer
        {
            head = tail = timer;
            return;
        }
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head); //这里名字没起好，起重复了，表示处理上面三种if情况，正常插入
    }
    void adjust_timer(util_timer *timer)    //调整对应的定时器在链表中的位置，有时候任务发生变化时会用
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        if (!tmp || (timer->expire < tmp->expire))  // 定时器在尾部或者超时值仍然小于下一个，不用调整
        {
            return;
        }
        if (timer == head)  //如果是头结点，重新插入？？
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        else                //将定时器取出来，重新插入
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    void del_timer(util_timer *timer)   //删除定时器
    {
        if (!timer)
        {
            return;
        }
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    void tick() //真正的SIGALRM信号的处理函数
    {
        if (!head)
        {
            return;
        }
        //printf( "timer tick\n" );
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        time_t cur = time(NULL);
        util_timer *tmp = head;
        while (tmp)
        {
            if (cur < tmp->expire)  // 如果head没超时，那么后面的节点也不可能超时，因为链表是按超时时间升序的
            {
                break;
            }

            // 否则head超时，断开这个head的socket，head 指向下一个
            tmp->cb_func(tmp->user_data);
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif
