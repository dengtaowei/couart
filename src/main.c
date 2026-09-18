#include "couart.h"

#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "serve") == 0)
        return couart_serve(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "mcp") == 0)
        return couart_mcp(argc - 1, argv + 1);
    return couart_cli(argc, argv);
}
