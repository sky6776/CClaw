#include "agent_conf.h"

#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    agent_app_init();

    while(1)
    {
        pause();
    }

    return 0;
}
