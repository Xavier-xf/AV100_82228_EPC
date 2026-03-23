#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "string.h"
#include <termios.h>
#include <stdbool.h>
#include "UartControl.h"

/***
**   日期:2022-05-30 17:12:23
**   作者: leo.liu
**   函数作用：打开串口设备
**   参数说明:
***/
static int UartOpenDevice(const char *dev)
{
	char path[128] = {0};
	sprintf(path, "/dev/%s", dev);
	/***** O_NOCTTY：告诉Unix这个程序不想成为“控制终端”控制的程序，不说明这个标志的话，任何输入都会影响你的程序。  *****/
	/***** O_NDELAY：告诉Unix这个程序不关心DCD信号线状态，即其他端口是否运行，不说明这个标志的话，该程序就会在DCD信号线为低电平时停止  *****/
	return open(path, O_RDWR | O_NOCTTY | O_NDELAY);
}

/***
**   日期:2022-05-30 17:07:38
**   作者: leo.liu
**   函数作用：波特率设置
**   参数说明:
***/
static bool UartSpeedSet(int fd, int speed)
{
	int speed_arr[] = {B115200, B57600, B38400, B19200, B9600, B4800, B2400, B1200, B300, B38400, B19200, B9600, B4800, B2400, B1200, B300};
	int name_arr[] = {115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200, 300, 38400, 19200, 9600, 4800, 2400, 1200, 300};
	struct termios opt;
	if (tcgetattr(fd, &opt) != 0)
	{
		printf("tcgetattr fd error \n");
		return false;
	}
	for (int i = 0; i < sizeof(speed_arr) / sizeof(int); i++)
	{
		if (speed == name_arr[i])
		{
			tcflush(fd, TCIOFLUSH);
			cfsetispeed(&opt, speed_arr[i]);
			cfsetospeed(&opt, speed_arr[i]);
			/*****  TCSANOW：立即执行而不等待数据发送或者接受完成。 *****/
			/*****  TCSADRAIN：等待所有数据传递完成后执行。 *****/
			/*****  TCSAFLUSH：Flush input and output buffers and make the change *****/
			if (tcsetattr(fd, TCSANOW, &opt) != 0)
			{
				printf("tcsetattr fd error \n");
				return false;
			}
			tcflush(fd, TCIOFLUSH);
			return true;
		}
	}
	return false;
}
/***
**   日期:2022-05-30 17:24:06
**   作者: leo.liu
**   函数作用：设置参数
**   参数说明:
***/
static bool UartParitySet(int fd, int data_bits, int stop_bits, int parity)
{
	struct termios opt;
	if (tcgetattr(fd, &opt) != 0)
	{
		printf("tcgetattr fd \n");
		return false;
	}
	/*****  一般必设置的标志 *****/
	opt.c_cflag |= (CLOCAL | CREAD);

	switch (data_bits) // 设置数据位数
	{
	case 7:
		opt.c_cflag &= ~CSIZE;
		opt.c_cflag |= CS7;
		break;
	case 8:
		opt.c_cflag &= ~CSIZE;
		opt.c_cflag |= CS8;
		break;
	default:
		printf("Unsupported data size.\n");
		return false;
	}

	switch (parity) // 设置校验位
	{
	case 'n':
	case 'N':
		opt.c_cflag &= ~PARENB; // 清除校验位
		opt.c_iflag &= ~INPCK;	// enable parity checking
		break;
	case 'o':
	case 'O':
		opt.c_cflag |= PARENB; // enable parity
		opt.c_cflag |= PARODD; // 奇校验
		opt.c_iflag |= INPCK;  // disable parity checking
		break;
	case 'e':
	case 'E':
		opt.c_cflag |= PARENB;	// enable parity
		opt.c_cflag &= ~PARODD; // 偶校验
		opt.c_iflag |= INPCK;	// disable pairty checking
		break;
	case 's':
	case 'S':
		opt.c_cflag &= ~PARENB; // 清除校验位
		opt.c_cflag &= ~CSTOPB; //??????????????
		opt.c_iflag |= INPCK;	// disable pairty checking
		break;
	default:
		printf("Unsupported parity.\n");
		return false;
	}
	switch (stop_bits) // 设置停止位
	{
	case 1:
		opt.c_cflag &= ~CSTOPB;
		break;
	case 2:
		opt.c_cflag |= CSTOPB;
		break;
	default:
		printf("Unsupported stopbits.\n");
		return false;
	}
	opt.c_cflag |= (CLOCAL | CREAD);
	/*****  有时候，在用write发送数据时没有键入回车，信息就发送不出去，这主要是因为我们在输入输出时是按照规范模式接收到回车或换行才发送，而更多情况下我们是不必键入回车或换行的。此时应转换到行方式输入，不经处理直接发送 *****/
	opt.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	opt.c_oflag &= ~OPOST;
	/*****  还存在这样的情况：发送字符0X0d的时候，往往接收端得到的字符是0X0a，原因是因为在串口设置中c_iflag和c_oflag中存在从NL-CR和CR-NL的映射，即串口能把回车和换行当成同一个字符，可以进行如下设置屏蔽之： *****/
	opt.c_oflag &= ~(ONLCR | OCRNL);
	opt.c_iflag &= ~(ICRNL | INLCR);
	/*****  c_cc数组的VSTART和VSTOP元素被设定成DC1和DC3，代表ASCII标准的XON和XOFF字符，如果在传输这两个字符的时候就传不过去，需要把软件流控制屏蔽，即： *****/
	opt.c_iflag &= ~(IXON | IXOFF | IXANY);
	tcflush(fd, TCIFLUSH);

	/*****  注意：控制符VTIME和VMIN之间有复杂的关系。VTIME定义要求等待的时间（百毫米，通常是unsigned char变量），而VMIN定义了要求等待的最小字节数（相比之下，read函数的第三个参数指定了要求读的最大字节数）。
如果VTIME=0，VMIN=要求等待读取的最小字节数，read必须在读取了VMIN个字节的数据或者收到一个信号才会返回。
如果VTIME=时间量，VMIN=0，不管能否读取到数据，read也要等待VTIME的时间量。
如果VTIME=时间量，VMIN=要求等待读取的最小字节数，那么将从read读取第一个字节的数据时开始计时，并会在读取到VMIN个字节或者VTIME时间后返回。
如果VTIME=0，VMIN=0，不管能否读取到数据，read都会立即返回 *****/
	opt.c_cc[VTIME] = 1; // 设置超时为15sec
	opt.c_cc[VMIN] = 0;	 // Update the opt and do it now
	if (tcsetattr(fd, TCSANOW, &opt) != 0)
	{
		printf("tcsetattr fd \n");
		return false;
	}
	return true;
}

/***
**   日期:2022-05-30 17:41:52
**   作者: leo.liu
**   函数作用：打开串口
**   参数说明:
***/
int UartOpen(char *dev, int speed, int data_bits, int stop_bits, int parity)
{
	int fd = UartOpenDevice(dev);
	if (fd < 0)
	{
		return -1;
	}
	if (UartSpeedSet(fd, speed) == false)
	{
		printf("set speed faield \n");
		usleep(1000 * 1000);
		return -1;
	}
	if (UartParitySet(fd, data_bits, stop_bits, parity) == false)
	{
		printf("set UartParitySet faield \n");
		usleep(1000 * 1000);
		return -1;
	}
	return fd;
}
/***
**   日期:2022-05-30 17:42:38
**   作者: leo.liu
**   函数作用：发送串口数据
**   参数说明:
***/
int UartWrite(int fd, char *data, int size)
{
	size = write(fd, data, size);
	usleep(1 * 1000);
	return size;
}

/***
**   日期:2022-05-30 17:46:47
**   作者: leo.liu
**   函数作用：读取串口数据
**   参数说明:
***/
int UartRead(int fd, char *data, int size)
{
	int num = 0;
#if 0
	// for (int i = 0; i < size; i++)
	// {
	// 	num += read(fd, &data[i], 1);
	// 	int count = 100;
	// 	while (count--)
	// 		;
	// }
#else
	num = read(fd, data, size);
#endif
	return num;
}
/***
**   日期:2022-05-30 17:46:55
**   作者: leo.liu
**   函数作用：关闭串口
**   参数说明:
***/
bool UartClose(int fd)
{
	close(fd);
	return true;
}

/***
**   日期:2022-09-21 17:46:55
**   作者: leo.wu
**   函数作用：清空串口清空输入输出缓存
**   参数说明:
***/
bool UartClear(int fd)
{
	tcflush(fd, TCIOFLUSH);
	return true;
}

/***
**   日期:2023-01-09 09:42:55
**   作者: leo.wu
**   函数作用：读取串口缓存
**   参数说明:
***/
#include <sys/ioctl.h>
int UartBufferSize(int fd)
{
	int bytes = 0;
	ioctl(fd, FIONREAD, &bytes);
	return bytes;
}