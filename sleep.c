#include "user/user.h"
#include "kernel/types.h"
#include "kernel/stat.h"
int
main(int argc, char* argv[])
{
    if(argc == 1)
    {
        fprintf(2, "error/n");
    }
    
    int t = atoi(argv[1]);
    sleep(t);
    exit(0);
}
