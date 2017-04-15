#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include "skynet_daemon.h"

//���pid
static int
check_pid(const char *pidfile) {
	int pid = 0;

	//���ļ�
	FILE *f = fopen(pidfile,"r");
	if (f == NULL)
		return 0;

	//��ʽ������
	int n = fscanf(f,"%d", &pid);
	fclose(f);

	if (n !=1 || pid == 0 || pid == getpid()) {
		return 0;
	}

	//int kill(pid_t pid, int sig);   ESRCH ����pid ��ָ���Ľ��̻�����鲻����
	/*
		1��pid>0 ���źŴ�������ʶ����Ϊpid �Ľ���.
		2��pid=0 ���źŴ�����Ŀǰ������ͬ����������н���
		3��pid=-1 ���źŹ㲥���͸�ϵͳ�����еĽ���
		4��pid<0 ���źŴ���������ʶ����Ϊpid ����ֵ�����н��̲��� sig ������źű�ſɲο���¼D

	*/
	if (kill(pid, 0) && errno == ESRCH)
		return 0;

	return pid;
}

//��pidд���ļ�
static int
write_pid(const char *pidfile) {
	FILE *f;
	int pid = 0;
	//�����ļ�
	int fd = open(pidfile, O_RDWR|O_CREAT, 0644);
	if (fd == -1) {
		fprintf(stderr, "Can't create %s.\n", pidfile);
		return 0;
	}
	
	//fdopen ����������һ���Ѿ��򿪵��ļ��������ϴ�һ����
	f = fdopen(fd, "r+");
	if (f == NULL) {
		fprintf(stderr, "Can't open %s.\n", pidfile);
		return 0;
	}

	//�ļ���
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

	//��ȡ����������
	pid = getpid();

	//д���ļ�
	if (!fprintf(f,"%d\n", pid)) {
		fprintf(stderr, "Can't write pid.\n");
		close(fd);
		return 0;
	}
	fflush(f);

	return pid;
}
  
//���� �ػ�����  daemon()����
int
daemon_init(const char *pidfile) {
	int pid = check_pid(pidfile);

	//����Ƿ��Ѿ�������skynet����
	if (pid) {
		fprintf(stderr, "Skynet is already running, pid = %d.\n", pid);
		return 1;
	}

#ifdef __APPLE__
	fprintf(stderr, "'daemon' is deprecated: first deprecated in OS X 10.5 , use launchd instead.\n");
#else
	//����Ϊ��̨����
	/*

	int daemon (int __nochdir, int __noclose);
	���__nochdir��ֵΪ0�����л�����Ŀ¼Ϊ��Ŀ¼��
	���__nocloseΪ0���򽫱�׼���룬����ͱ�׼�����ض���/dev /null��

	*/
	if (daemon(1,0)) {
		fprintf(stderr, "Can't daemonize.\n");
		return 1;
	}
#endif

//��pidд���ļ�
	pid = write_pid(pidfile);

	if (pid == 0) {
		return 1;
	}

	return 0;
}

//�˳�����
int
daemon_exit(const char *pidfile) {
	return unlink(pidfile);
}
