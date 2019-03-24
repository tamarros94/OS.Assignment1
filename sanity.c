#include "types.h"
//#include "stat.h"
#include "user.h"

//void test()
//{
//    int pid = fork();
//    //this is the child
//    if (pid == 0) {
//        sleep(300);
//        exit(5);
//    }
//    else {
//        int* status = malloc(1);
//        wait(status);
//        printf(1, "exit status after is %d\n",*status);
//    }
//
//    exit(0);
//}
//

int main(void)
{
int pid;
int first_status=0;
int second_status=0;
int third_status=0;

pid= fork();
if( pid > 0){

    first_status = detach(pid);  // status = 0
    second_status = detach(pid); // status = -1
    third_status = detach(pid + 1);  // status = -1

}
    printf(1,"first status is : %d\n", first_status);
    printf(1,"second status is : %d\n", second_status);
    printf(1,"third status is : %d\n", third_status);

    exit(0);

}