#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>


#define  SERVER_PORT 5566  //  define the defualt connect port id
#define  CLIENT_PORT ((20001+rand())%65536)  //  define the defualt client port as a random port
#define  BUFFER_SIZE 2056
#define  REUQEST_MESSAGE "welcome to connect the server.\n"

#define HTTP_C_MAGIC        0x10293874
#define HTTP_C_VERSION      0x1
#define HTTP_C_REQ          0x1
#define HTTP_C_RESP         0x2
#define HTTP_C_HAND         0x3
#define HTTP_C_SYNC         0x4
#define HTTP_C_TUNNELREQ    0x5
#define HTTP_C_TUNNELRESP   0x6
#define HTTP_C_HEADER_LEN   sizeof(http_c_header)

typedef struct _http_c_header {
    unsigned int    magic;
    unsigned short  version;
    unsigned short  type;
    unsigned short  length;     /* Length include the header */
    unsigned short  seq;
    unsigned int    reserved;   /* the first byte is use to identify TODO */
}__attribute__ ((packed)) http_c_header;

char* createBuf(int* total_len) {
    char* pbuf;
    unsigned short len;
    char* str = "hello word";
    http_c_header h, *header;
    header = &h;

    pbuf = (char*)malloc(BUFFER_SIZE);
    header->magic = htonl(HTTP_C_MAGIC);
    header->version = htons(HTTP_C_VERSION);
    header->type = htons(HTTP_C_REQ);
    len = (unsigned short)(HTTP_C_HEADER_LEN + strlen(str) + 1);
    header->length = htons(len);
    header->seq = htons(0x66);
    header->reserved = 0;
    memcpy(pbuf, header, HTTP_C_HEADER_LEN);
    strcpy(pbuf+HTTP_C_HEADER_LEN, str);

    *total_len = len;

    return pbuf;
}

void  usage(char* name)
{
       printf( " usage: %s IpAddr\n " ,name);
}

int  main(int argc, char** argv)
{
       int  clifd, n, optval, length = 0;
       struct  sockaddr_in servaddr,cliaddr;
       socklen_t socklen  =   sizeof (servaddr);
       char  buf[BUFFER_SIZE];
       char* pbuf;

        if (argc < 2 )
         {
              usage(argv[ 0 ]);
              exit( 1 );
       }

       if ((clifd  =  socket(AF_INET,SOCK_STREAM, 0 ))  <   0 )
         {
             printf( " create socket error!\n " );
             exit( 1 );
       }

       //setsockopt(clifd, SOL_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));

       srand(time(NULL)); // initialize random generator

       bzero( & cliaddr, sizeof (cliaddr));
       cliaddr.sin_family  =  AF_INET;
       cliaddr.sin_port  =  htons(CLIENT_PORT);
       cliaddr.sin_addr.s_addr  =  htons(INADDR_ANY);

       bzero( & servaddr, sizeof (servaddr));
       servaddr.sin_family  =  AF_INET;
       inet_aton(argv[ 1 ], & servaddr.sin_addr);
       servaddr.sin_port  =  htons(SERVER_PORT);
      // servaddr.sin_addr.s_addr = htons(INADDR_ANY);

       if  (bind(clifd, (struct sockaddr* ) &cliaddr, sizeof (cliaddr)) < 0 )
       {
              printf( " bind to port %d failure!\n " ,CLIENT_PORT);
              exit( 1 );
       }

        if (connect(clifd,( struct  sockaddr * ) & servaddr, socklen)  <   0 )
       {
              printf( " can't connect to %s!\n ", argv[ 1 ]);
              exit( 1 );
       }

        pbuf = createBuf(&length);
        n = send(clifd, pbuf, length, 0);
        free(pbuf);
        if(n != length) {
            printf("send error \n");
            exit(1);
        }
        printf("send pbuf ok\n");
        setsockopt(clifd, IPPROTO_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));
        //fflush(clifd);
        //shutdown(clifd, SHUT_WR);

       length  =  recv(clifd, buf, BUFFER_SIZE, 0);
        if  (length < 0)
        {
              printf( " error comes when recieve data from server %s! ", argv[1] );
              exit( 1 );
       }

       printf( "From server %s: len=%d, %s", argv[1], length, buf+16);

       length  =  recv(clifd, buf, BUFFER_SIZE, 0);
        if  (length < 0)
        {
              printf( " error comes when recieve data from server %s! ", argv[1] );
              exit( 1 );
       }
       printf( "From server222 %s: len=%d, %s", argv[1], length, buf+16);

       close(clifd);
       return 0;
}
