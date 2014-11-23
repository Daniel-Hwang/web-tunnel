#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <poll.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "list.h"

struct num_list
{
    struct list_head node;
    int num;
};

void print_list(struct list_head* plist)
{
    struct list_head* p;
    struct num_list* pn;

    fprintf(stderr, "n: ");
    list_for_each(p, plist) {
        pn = list_entry(p, struct num_list, node);
        fprintf(stderr, " %d", pn->num);
    }
    fprintf(stderr, "\n");
}

static struct num_list* next_pn(struct list_head* plist)
{
    struct num_list* pn;
    if(!list_empty(plist))
    {
        list_for_each_entry(pn, plist, node) {
            break;
        }
        //list_del(&buf_info->node);
    }

    return pn;
}

static void find_test(struct list_head* plist, int n) {
    struct num_list* pn;

    list_for_each_entry(pn, plist, node) {
        if(pn->num == n) {
            break;
        }
    }

    if(&pn->node == plist) {
        fprintf(stderr, "Not found n=%d\n", n);
        return;
    }
    if(NULL == pn) {
        fprintf(stderr, "Got num pn\n");
        return;
    }
    fprintf(stderr, "found num %d\n", pn->num);
}

void test_list()
{
    struct list_head list1, list2;
    struct num_list* pn;

    INIT_LIST_HEAD(&list1);
    INIT_LIST_HEAD(&list2);

    fprintf(stderr, "started\n");
    print_list(&list1);
    print_list(&list2);

    fprintf(stderr, "list add test\n\n");
    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 4;
    list_add(&pn->node, &list1);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 5;
    list_add(&pn->node, &list1);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 6;
    list_add(&pn->node, &list1);

    print_list(&list1);

    find_test(&list1, 5);
    find_test(&list1, 13);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 12;
    list_add_tail(&pn->node, &list1);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 13;
    list_add_tail(&pn->node, &list1);
    print_list(&list1);

    fprintf(stderr, "test splice\n\n");
    list_splice_init(&list1, &list2);
    list_splice_init(&list1, &list2);
    list_splice_init(&list1, &list2);
    print_list(&list1);
    print_list(&list2);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 116;
    list_add_tail(&pn->node, &list1);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 117;
    list_add_tail(&pn->node, &list1);
    list_splice_init(&list1, &list2);
    list_splice_init(&list1, &list2);
    print_list(&list1);
    print_list(&list2);

    fprintf(stderr, "tail init\n\n");
    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 118;
    list_add_tail(&pn->node, &list1);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 119;
    list_add_tail(&pn->node, &list1);
    list_splice_tail_init(&list1, &list2);
    list_splice_tail_init(&list1, &list2);

    pn = (struct num_list*)malloc(sizeof(struct num_list));
    pn->num = 120;
    list_add_tail(&pn->node, &list2);

    print_list(&list1);
    print_list(&list2);

    pn = next_pn(&list2);
    fprintf(stderr, "test next\n %d ", pn->num);

    pn = next_pn(&list2);
    fprintf(stderr, "test next\n %d ", pn->num);
    list_del(&pn->node);

    pn = next_pn(&list2);
    fprintf(stderr, "test next\n %d ", pn->num);
    print_list(&list2);
}

int main() {
    test_list();
    return 0;
}
