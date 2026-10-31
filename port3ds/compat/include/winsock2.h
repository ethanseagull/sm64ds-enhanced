// Winsock on 3DS: types and constants only. The HAL's network code never calls
// Winsock directly; it loads ws2_32.dll into a table of function pointers, and
// our LoadLibraryA refuses every DLL, so those stay idle (the game's own
// wireless timeout kicks in). Multiplayer isn't supported; it could be done
// later with libctru's BSD sockets. Values match Winsock's.
#ifndef PORT3DS_COMPAT_WINSOCK2_H
#define PORT3DS_COMPAT_WINSOCK2_H
#include "windows.h"

#define WSAAPI

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned int SOCKET;
#define INVALID_SOCKET ((SOCKET)~0u)
#define SOCKET_ERROR (-1)

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_RCVBUF 0x1002
#define SO_SNDBUF 0x1001
#define TCP_NODELAY 0x0001

#define INADDR_ANY ((u_long)0x00000000)
#define INADDR_LOOPBACK ((u_long)0x7f000001)
#define INADDR_NONE ((u_long)0xffffffff)

#define IOCPARM_MASK 0x7f
#define IOC_VOID 0x20000000
#define IOC_OUT 0x40000000
#define IOC_IN 0x80000000
#define IOC_VENDOR 0x18000000
#define _WSAIOW(x, y) (IOC_IN | (x) | (y))
#define _IOW(x, y, t) (IOC_IN | (((long)sizeof(t) & IOCPARM_MASK) << 16) | ((x) << 8) | (y))
#define FIONBIO _IOW('f', 126, u_long)

#define WSABASEERR 10000
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSANOTINITIALISED (WSABASEERR + 93)

struct in_addr {
    union {
        struct { u_char s_b1, s_b2, s_b3, s_b4; } S_un_b;
        struct { u_short s_w1, s_w2; } S_un_w;
        u_long S_addr;
    } S_un;
};
#define s_addr S_un.S_addr

struct sockaddr {
    u_short sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    short sin_family;
    u_short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct hostent {
    char *h_name;
    char **h_aliases;
    short h_addrtype;
    short h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
} WSADATA, *LPWSADATA;

typedef struct _WSAOVERLAPPED {
    DWORD Internal, InternalHigh, Offset, OffsetHigh;
    HANDLE hEvent;
} WSAOVERLAPPED, *LPWSAOVERLAPPED;
typedef void (*LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

#endif
