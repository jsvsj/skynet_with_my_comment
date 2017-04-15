#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include "skynet_daemon.h"

//检查pid
static int
check_pid(const char *pidfile) {
	int pid = 0;

	//打开文件
	FILE *f = fopen(pidfile,"r");
	if (f == NULL)
		return 0;

	//格式化输入
	int n = fscanf(f,"%d", &pid);
	fclose(f);

	if (n !=1 || pid == 0 || pid == getpid()) {
		return 0;
	}

	//int kill(pid_t pid, int sig);   ESRCH 参数pid 所指定的进程或进程组不存在
	/*
		1、pid>0 将信号传给进程识别码为pid 的进程.
		2、pid=0 将信号传给和目前进程相同进程组的所有进程
		3、pid=-1 将信号广播传送给系统内所有的进程
		4、pid<0 将信号传给进程组识别码为pid 绝对值的所有进程参数 sig 代表的信号编号可参考附录D

	*/
	if (kill(pid, 0) && errno == ESRCH)
		return 0;

	return pid;
}

//将pid写入文件
static int
write_pid(const char *pidfile) {
	FILE *f;
	int pid = 0;
	//创建文件
	int fd = open(pidfile, O_RDWR|O_CREAT, 0644);
	if (fd == -1) {
		fprintf(stderr, "Can't create %s.\n", pidfile);
		return 0;
	}
	
	//fdopen 函数用于在一个已经打开的文件描述符上打开一个流
	f = fdopen(fd, "r+");
	if (f == NULL) {
		fprintf(stderr, "Can't open %s.\n", pidfile);
		return 0;
	}

	//文件锁
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		int n = fscanf(f, "%d", &pid);
		fclose(f);
		if (n != 1) {
			fprintf(stderr, "Can't lock and read pidfile.\n");
		} else {
			fprintf(stderr, "Can't lock pidfile, lock is held by pid %d.\n", pid);
		}
		return 0;
	}

	//获取进程描述符
	pid = getpid();

	//写入文件
	if (!fprintf(f,"%d\n", pid)) {
		fprintf(stderr, "Can't write pid.\n");
		close(fd);
		return 0;
	}
	fflush(f);

	return pid;
}
  
//启动 守护进程  daemon()函数
int
daemon_init(const char *pidfile) {
	int pid = check_pid(pidfile);

	//检测是否已经启动过skynet进程
	if (pid) {
		fprintf(stderr, "Skynet is already running, pid = %d.\n", pid);
		return 1;
	}

#ifdef __APPLE__
	fprintf(stderr, "'daemon' is deprecated: first deprecated in OS X 10.5 , use launchd instead.\n");
#else
	//设置为后台进程
	/*

	int daemon (int __nochdir, int __noclose);
	如果__nochdir的值为0，则将切换工作目录为根目录；
	如果__noclose为0，则将标准输入，输出和标准错误都重定向到/dev /null。

	*/
	if (daemon(1,0)) {
		fprintf(stderr, "Can't daemonize.\n");
		return 1;
	}
#endif

//将pid写入文件
	pid = write_pid(pidfile);

	if (pid == 0) {
		return 1;
	}

	return 0;
}

//退出进程
int
daemon_exit(const char *pidfile) {
	return unlink(pidfile);
}
