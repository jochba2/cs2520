#ifndef _THREAD_H
#define _THREAD_H
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdexcept>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

class Monitor{
public:
	Monitor(){
		pthread_mutexattr_t mutexattr;
		pthread_condattr_t condattr;

		pthread_mutexattr_init(&mutexattr);
		pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);

		pthread_condattr_init(&condattr);
		pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_PRIVATE);

		pthread_mutex_init(&mutex, &mutexattr);
		pthread_cond_init(&condition_variable, &condattr);

		pthread_mutexattr_destroy(&mutexattr);
		pthread_condattr_destroy(&condattr);
	}
	~Monitor(){
		pthread_cond_destroy(&condition_variable);
		pthread_mutex_destroy(&mutex);
	}
	void lock(){
		pthread_mutex_lock(&mutex);
	}
	void unlock(){
		pthread_mutex_unlock(&mutex);
	}
	void wait(){
		pthread_cond_wait(&condition_variable, &mutex);
	}
	bool wait(int timeout){
		timespec abstime;
		abstime.tv_sec = time(NULL) + timeout;
		abstime.tv_nsec = 0;
		int ret = pthread_cond_timedwait(&condition_variable, &mutex, &abstime);
		return ret != ETIMEDOUT;
	}
	void notify(){
		pthread_cond_signal(&condition_variable);
	}
	void notifyAll(){
		pthread_cond_broadcast(&condition_variable);
	}
private:
	pthread_mutex_t mutex;
	pthread_cond_t condition_variable;
};

static inline void *main_thread_function(void* param);

class Thread{
public:
	Thread(){
	    _is_stopped = true;
	}
	void start(){
		_to_stop_signal = false;
		_is_stopped = false;
		int iret;
		pthread_attr_t attrs;
		pthread_attr_init(&attrs);
		iret = pthread_create(&_thread_id, NULL, main_thread_function, static_cast<void*> (this));
		if(iret)
		{
			char erno[20];
			sprintf(erno, "%i", iret);
			std::string err("Error - could not start thread :");
			err = err + erno;
			throw std::runtime_error(err.c_str());
		}

		pthread_attr_destroy(&attrs);
    }
	virtual ~Thread(){
		terminate();
	}
	void join(){
		monitor.lock();
		bool tojoin = !_is_stopped;
		monitor.unlock();
		if(tojoin)
            pthread_join(_thread_id, NULL);
	}
	void signalStop(){
		monitor.lock();
		_to_stop_signal = true;
		monitor.notify();
		monitor.unlock();
	}
	void stop(){
		signalStop();
		join();
	}
	void terminate(){
		monitor.lock();
		_to_stop_signal = true;
		bool toTerminate = !_is_stopped;
		monitor.notify();
		monitor.unlock();
	    if(toTerminate)
            pthread_cancel(_thread_id);
        _is_stopped = true;
	}
	bool toStop(){
		monitor.lock();
		bool s = _to_stop_signal;
		monitor.unlock();
		return s;
	}
	friend void *main_thread_function(void* param);
protected:
	virtual void execute()=0;
	Monitor monitor;
private:
	pthread_t _thread_id;
	bool _to_stop_signal;
	bool _is_stopped;
};

static inline void *main_thread_function(void* param){
	Thread *thread = static_cast<Thread*>(param);
	thread->execute();
	thread->monitor.lock();
	thread->_is_stopped = true;
	thread->monitor.unlock();
	return NULL;
}
#endif
