#ifndef __LIST_ENTRY_H__
#define __LIST_ENTRY_H__

typedef struct list_entry_s {
	struct list_entry_s *next;
	struct list_entry_s *prev;
} list_entry_t;

//
//  void
//  initialize_list_head(
//      list_entry_t* list_head
//      );
//

#define initialize_list_head(list_head) (\
    (list_head)->next = (list_head)->prev = (list_head))

//
//  bool_t
//  is_list_empty(
//      list_entry_t* list_head
//      );
//

#define is_list_empty(list_head) \
    ((list_head)->next == (list_head))

//
//  void
//  remove_entry_list(
//      list_entry_t* entry
//      );
//

#define remove_entry_list(entry) {\
    list_entry_t* __old_prev;\
    list_entry_t* __old_next;\
    __old_next = (entry)->next;\
    __old_prev = (entry)->prev;\
    __old_prev->next = __old_next;\
    __old_next->prev = __old_prev;\
    }

//
//  void
//  insert_tail_list(
//      list_entry_t* list_head,
//      list_entry_t* entry
//      );
//

#define insert_tail_list(list_head, entry) {\
    list_entry_t* __old_prev;\
    list_entry_t* __old_list_head;\
    __old_list_head = (list_head);\
    __old_prev = __old_list_head->prev;\
    (entry)->next = __old_list_head;\
    (entry)->prev = __old_prev;\
    __old_prev->next = (entry);\
    __old_list_head->prev = (entry);\
    }

#endif // __LIST_ENTRY_H__
