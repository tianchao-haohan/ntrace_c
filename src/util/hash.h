#ifndef __WDM_AGENT_HASH_H__
#define __WDM_AGENT_HASH_H__

#include <stdlib.h>
#include "list.h"

typedef struct _hlistNode hlistNode;
typedef hlistNode *hlistNodePtr;

struct _hlistNode {
    hlistNodePtr next;
    hlistNodePtr *pprev;
};

typedef struct _hlistHead hlistHead;
typedef hlistHead *hlistHeadPtr;

struct _hlistHead {
    hlistNodePtr first;
};

/* init hash list head */
#define HLIST_HEAD(head) hlistHead head = {.first = NULL}
#define INIT_HLIST_HEAD(head) ((head)->first =  NULL)

static inline void
INIT_HLIST_NODE (hlistNodePtr node) {
    node->next = NULL;
    node->pprev = NULL;
}

typedef void (*hashFreeFun) (void *item);
typedef int (*hashForEachItemDoFun) (void *item, void *args);
typedef int (*hashForEachItemDelIfFun) (void *item, void *args);
typedef void * (*hashForEachItemCheckFun) (void *item, void *args);

typedef struct _hashItem hashItem;
typedef hashItem *hashItemPtr;

struct _hashItem {
    char *key;                  /**< Key string used to compute hash value */
    size_t index;               /**< Index in hash table */
    void *data;                 /**< Opaque item value */
    hashFreeFun freeFun;        /**< Mem free func */
    hlistNode node;             /**< Hash list node */
};

typedef struct _hashTable hashTable;
typedef hashTable *hashTablePtr;

struct _hashTable {
    size_t currSize;            /**< Num of items in hash table */
    size_t totalSize;           /**< Size of hash table */
    size_t limit;               /**< Limit of hash table */
    hlistHeadPtr heads;         /**< Array of hlist_head */
};

static inline int
hlistIsEmpty (const hlistHeadPtr head) {
    return !head->first;
}

/* delete hash node from hash list */
static inline void
hlistDel (hlistNodePtr node) {
    hlistNodePtr next = node->next;
    hlistNodePtr *pprev = node->pprev;
    *pprev = next;
    if (next)
        next->pprev = pprev;
    INIT_HLIST_NODE (node);
}

/* add hash node to the head */
static inline void
hlistAddHead (hlistNodePtr node, hlistHeadPtr head) {
    hlistNodePtr first = head->first;
    node->next = first;
    head->first = node;
    node->pprev = &head->first;
    if(first)
        first->pprev = &node->next;
}

/*
 * @brief add hash node before specified node
 *
 * @param node node to add
 * @param nnode the specified node to insert before
 */
static inline void
hlistAddBefore (hlistNodePtr node, hlistNodePtr nnode) {
    node->pprev = nnode->pprev;
    *(node->pprev) = node;
    node->next = nnode;
    nnode->pprev = &node->next;
}

/*
 * @brief add hash node after specified node
 *
 * @param node node to add
 * @param pnode the specified node to insert after
 */
static inline void
hlistAddAfter (hlistNodePtr node, hlistNodePtr pnode) {
    node->next = pnode->next;
    pnode->next = node;
    node->pprev = &pnode->next;
    if(node->next)
        node->next->pprev = &node->next;
}

/*
 * @brief move a hash list from one list head to another. Fixup the pprev
 *        reference of the first entry if it exists
 *
 * @param old the old hash list head
 * @param new the new hash list head
 */
static inline void
hlistMoveList (hlistHeadPtr old, hlistHeadPtr new) {
    new->first = old->first;
    if(new->first)
        new->first->pprev = &new->first;
    old->first = NULL;
}

#define hlistEntry(ptr, type, member)           \
    containerOfMember(ptr, type, member)
#define hlistForEach(pos, head)                     \
    for (pos = (head)->first; pos; pos = pos->next)
/*
 * @brief iterate over a hash list of given type
 *
 * @param tpos the type * to use as a loop cursor
 * @param pos the struct hlist_node & to use as a loop cursor
 * @param head the head of hash list
 * @param member the name the hlist_node within the type
 */
#define hlistForEachEntry(tpos, pos, head, member)                      \
    for (tpos = NULL, pos = (head)->first;                              \
         pos && ({tpos = hlistEntry (pos, typeof (*tpos), member); 1;}); \
         pos = pos->next)

/* iterate over list of given type safe against removal of list entry */
#define hlistForEachEntrySafe(tpos, pos, n, head, member)               \
    for (tpos = NULL, pos = (head)->first;                              \
         pos && ({n = pos->next; 1;}) && ({tpos = hlistEntry (pos, typeof (*tpos), member); 1;}); \
         pos = n)

#define hlistForEachEntryFrom(tpos, pos, member)                        \
    for (; pos && ({tpos = hlistEntry (pos, typeof (*tpos), member); 1;}); \
         pos = pos->next)

#define hlistForEachEntrySafeFrom(tpos, pos, n, member)                 \
    for (; pos && ({n = pos->next; 1;}) &&                              \
                 ({tpos = hlistEntry (pos, typeof (*tpos), member); 1;}); \
         pos = n)

/*========================Interfaces definition============================*/
hashTablePtr
hashNew (u_int hashSize);
void
hashDestroy (hashTablePtr *htblPtr);
int
hashInsert (hashTablePtr htbl, const char *key, void *data, hashFreeFun fun);
int
hashUpdate (hashTablePtr htbl, const char *key, void *data, hashFreeFun fun);
int
hashDel (hashTablePtr htbl, const char *key);
void *
hashLookup (hashTablePtr htbl, const char *key);
int
hashRename (hashTablePtr htbl, const char *old_key, const char *new_key);
size_t
hashSize (hashTablePtr htbl);
size_t
hashLimit (hashTablePtr htbl);
int
hashForEachItemDo (hashTablePtr htbl, hashForEachItemDoFun fun, void *args);
void
hashForEachItemDelIf (hashTablePtr htbl, hashForEachItemDelIfFun fun, void *args);
void *
hashForEachItemCheck (hashTablePtr htbl, hashForEachItemCheckFun fun, void *args);
/*=======================Interfaces definition end=========================*/

#endif /* __WDM_AGENT_HASH_H__ */
