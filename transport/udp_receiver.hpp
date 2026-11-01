#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h> // structure library for internet addressing
#include <sys/socket.h> // required for all socket functions
#include <sys/types.h>  //
#include <unistd.h>     // POSIX (spec rulebook), we use it for close() on macOS
