#include "kernel/types.h"
#include "user/user.h"

int
main(int argc,char* argv[])//参数 argc 是命令行总参数的个数，参数 argv[] 是 argc 个参数，其中第 0 个参数是程序的全名，其他的参数是命令行后面跟的用户输入的参数
{   
    if(argc != 2){
        write(2,"Usage:sleep time\n",strlen("Usage:sleep time\n"));
        exit(1);
    }
    int time = atoi(argv[1]);//把字符串转换成整型数
    sleep(time);
    exit(0);
}