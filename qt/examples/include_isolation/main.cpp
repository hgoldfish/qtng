// Must not compile when linked against qtnetworkng (core headers are PRIVATE).
#include <qtng/socket.h>

int main()
{
    return 0;
}
