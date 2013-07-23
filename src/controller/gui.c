
#include "ch.h"
#include "gui.h"
#include "touch.h"
#include "message.h"


typedef struct widget_stack_elem_s {
  widget_t* widget;
  struct widget_stack_elem_s* next;
} widget_stack_elem_t;


static msg_t gui_thread_func(void* arg);

static void dispatch_touch(touch_msg_t* event);
static void dispatch_push_screen(widget_t* screen);
static void dispatch_pop_screen(void);
static void gui_dispatch(msg_id_t id, void* msg_data, void* user_data);
static void dispatch_msg(widget_t* w, msg_id_t id, void* msg_data);


static WORKING_AREA(wa_gui_thread, 1024);
static Thread* gui_thread;
static widget_t* touch_capture_widget;
static widget_stack_elem_t* screen_stack = NULL;
static systime_t last_paint_time;


void
gui_init()
{
  gui_thread = chThdCreateStatic(wa_gui_thread, sizeof(wa_gui_thread), NORMALPRIO, gui_thread_func, NULL);

  msg_subscribe(MSG_TOUCH_INPUT, gui_thread, gui_dispatch, NULL);
  msg_subscribe(MSG_GUI_PUSH_SCREEN, gui_thread, gui_dispatch, NULL);
  msg_subscribe(MSG_GUI_POP_SCREEN, gui_thread, gui_dispatch, NULL);
}

void
gui_push_screen(widget_t* screen)
{
  msg_broadcast(MSG_GUI_PUSH_SCREEN, screen);
}

void
gui_pop_screen()
{
  msg_broadcast(MSG_GUI_POP_SCREEN, NULL);
}

void
gui_acquire_touch_capture(widget_t* widget)
{
  touch_capture_widget = widget;
}

void
gui_release_touch_capture(void)
{
  touch_capture_widget = NULL;
}

void
gui_msg_subscribe(msg_id_t id, widget_t* w)
{
  if (w == NULL)
    return;

  msg_subscribe(id, gui_thread, gui_dispatch, w);
}

void
gui_msg_unsubscribe(msg_id_t id, widget_t* w)
{
  if (w == NULL)
    return;

  msg_unsubscribe(id, gui_thread, gui_dispatch, w);
}

static msg_t
gui_thread_func(void* arg)
{
  (void)arg;

  while (1) {
    Thread* tp = chMsgWaitTimeout(MS2ST(50));
    if (tp != NULL) {
      thread_msg_t* msg = (thread_msg_t*)chMsgGet(tp);

      gui_dispatch(msg->id, msg->msg_data, msg->user_data);

      chMsgRelease(tp, 0);
    }

    if ((chTimeNow() - last_paint_time) >= MS2ST(100)) {
      if (screen_stack != NULL)
        widget_paint(screen_stack->widget);
      last_paint_time = chTimeNow();
    }
  }
  return 0;
}

static void
gui_dispatch(msg_id_t id, void* msg_data, void* user_data)
{
  if (user_data != NULL) {
    dispatch_msg(user_data, id, msg_data);
  }
  else {
    switch(id) {
    case MSG_GUI_PUSH_SCREEN:
      dispatch_push_screen(msg_data);
      break;

    case MSG_GUI_POP_SCREEN:
      dispatch_pop_screen();
      break;

    case MSG_TOUCH_INPUT:
      dispatch_touch(msg_data);
      break;

    default:
      break;
    }
  }
}

static void
dispatch_touch(touch_msg_t* touch)
{
  widget_t* dest_widget = NULL;

  if (touch_capture_widget != NULL)
    dest_widget = touch_capture_widget;
  else if (screen_stack != NULL) {
    dest_widget = widget_hit_test(screen_stack->widget, touch->calib);
  }

  if (dest_widget != NULL) {
    touch_event_t te = {
        .id = touch->touch_down ? EVT_TOUCH_DOWN : EVT_TOUCH_UP,
        .widget = dest_widget,
        .pos = touch->calib,
    };
    widget_dispatch_event(dest_widget, (event_t*)&te);
  }
}

static void
dispatch_push_screen(widget_t* screen)
{
  widget_stack_elem_t* stack_elem = malloc(sizeof(widget_stack_elem_t));
  stack_elem->widget = screen;
  stack_elem->next = screen_stack;
  screen_stack = stack_elem;

  widget_invalidate(screen_stack->widget);
}

static void
dispatch_pop_screen()
{
  if ((screen_stack != NULL) &&
      (screen_stack->next != NULL)) {
    widget_destroy(screen_stack->widget);

    screen_stack = screen_stack->next;

    widget_invalidate(screen_stack->widget);
  }
}

static void
dispatch_msg(widget_t* w, msg_id_t id, void* msg_data)
{
  msg_event_t event = {
      .id = EVT_MSG,
      .widget = w,
      .msg_id = id,
      .msg_data = msg_data
  };
  widget_dispatch_event(w, (event_t*)&event);
}