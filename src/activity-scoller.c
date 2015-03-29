#include <pebble.h>
#include <inttypes.h>
#include <time.h>

#define NUM_MENU_ICONS 4
#define NUM_DEFAULT_ITEMS 3
#define NUM_SECTIONS 2
#define SAMPLE_BATCH 18
#define STOP_SIGNAL 0xFFFF
#define START_SIGNAL 0x0000

static Window *window;

static MenuLayer *menu_layer;

static GBitmap *menu_icons[NUM_MENU_ICONS];

static TextLayer *text_layer;

static const char* const activity_list[] = {"Running", "Walking", "Custom Activity"};

static const char* const TIME_FORMAT_RECORDING =  "%02d:%02d:%02d Recording";

static const char* const TIME_FORMAT_IDLE =  "%02d:%02d:%02d Stop";

// constant to recognize phone signal to save activity
static const uint32_t const SAVE_ACTIVITY = 3;

// constant to recognize phone signal to select activity but don't save
static const uint32_t const NO_SAVE_ACTIVITY = 2;

static int num_save_items = 0;

// startting time for the counter
static int s_uptime = 0;

static char s_uptime_buffer[48];

// check if we are currently recording data
bool is_recording;

// Data logging session
DataLoggingSessionRef accel_log;

// struct for Accelarator
typedef struct {
  int16_t x;
  int16_t y;
  int16_t z;
  uint8_t label;
} AccelDataMod;

// global variable for storing current value of accelerometer
static AccelDataMod accel_mod[SAMPLE_BATCH];

// current activity label, just need index, the phone can take care of the rest
static uint8_t current_label;

// waiting for phone to send signal back that activity is selected
static bool is_received_activity;

// create new log
void init_dlog(void) {
  accel_log = data_logging_create(
	 /* tag */           42,
     /* DataLogType */ DATA_LOGGING_BYTE_ARRAY,
     /* length */        sizeof(AccelDataMod)*SAMPLE_BATCH,
     /* resume */        false );
   APP_LOG(APP_LOG_LEVEL_DEBUG, "==> create log");
   accel_service_set_sampling_rate(ACCEL_SAMPLING_50HZ);
}

// boolean to denote dumping process
static bool is_dumping;

// how long we have been recording
static time_t startTime;

void send_signal(int signal) {
  DictionaryIterator *iter;

  app_message_outbox_begin(&iter);
  
  dict_write_int32(iter, signal, 1);

  app_message_outbox_send();

}



const char* getStatus(DataLoggingResult result)
{
   switch (result)
   {
   case DATA_LOGGING_SUCCESS:
	    return "Success";
   case DATA_LOGGING_BUSY:
	 return "busy";
   case DATA_LOGGING_FULL:
	 return "full";
   case DATA_LOGGING_NOT_FOUND:
	 return "not found";
   case DATA_LOGGING_CLOSED:
	 return "closed";
   case DATA_LOGGING_INVALID_PARAMS:
	 return "invalid";
   default: return "fup";

   }
}

static void accel_data_handler(AccelData *data, uint32_t num_samples) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "receiving %"PRIu32" %d\n", num_samples, !is_dumping);
  if ((num_samples >= SAMPLE_BATCH) && (!is_dumping)){
	uint32_t count = 0;
	AccelDataMod *modData = accel_mod;
	while (count < SAMPLE_BATCH) {
	  modData->x = data->x;
	  modData->y = data->y;
	  modData->z = data->z;
	  modData->label = current_label;
	  char buffer[32];
	  snprintf(buffer, sizeof(buffer), "dump %d ,%d, %d", data->x, data->y, data->z);
	  APP_LOG(APP_LOG_LEVEL_DEBUG, buffer);
	  data ++; // move to the next data point of AccelData struct
	  modData ++; // move to the next data point of AccelDataMod struct
	  count += 1;
	}
	DataLoggingResult r = data_logging_log(accel_log, accel_mod, 1);
	APP_LOG(APP_LOG_LEVEL_DEBUG, "STATUS %s", getStatus(r));
	if (r != DATA_LOGGING_SUCCESS) {
	  time_t offTime;
	  offTime = time(NULL);
	  APP_LOG(APP_LOG_LEVEL_DEBUG, "Status: %s Starting time is %ld, Current time is %ld", getStatus(r), startTime, offTime);
	}
  }
}

void initialize_icon() {
  int num_menu_icons = 0;
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_RUNNING);
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_WALKING);
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_ADD_ACTIVITY);
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_SAVED_ACTIVITY);
}

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
   switch (section_index) {
     case 0:
       return NUM_DEFAULT_ITEMS;

     case 1:
       return num_save_items;

     default:
       return 0;
   }
}

// A callback is used to specify the height of the section header
static int16_t menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  // This is a define provided in pebble.h that you may use for the default height
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void menu_draw_header_callback(GContext* ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  // Determine which section we're working with
  switch (section_index) {
    case 0:
      menu_cell_basic_header_draw(ctx, cell_layer, "Default Activity");
      break;

    case 1:
      menu_cell_basic_header_draw(ctx, cell_layer, "Saved Activity");
      break;
  }
}

static void menu_draw_row_callback(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  // Use the row to specify which item we'll draw
  menu_cell_basic_draw(ctx, cell_layer, activity_list[cell_index->row], NULL, menu_icons[cell_index->row]);
}


static void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "==> ticking log");

    // Get time since launch
    int seconds = s_uptime % 60;
    int minutes = (s_uptime % 3600) / 60;
    int hours = s_uptime / 3600;

    // Update the TextLayer
    snprintf(s_uptime_buffer, sizeof(s_uptime_buffer), TIME_FORMAT_RECORDING, hours, minutes, seconds);
    text_layer_set_text(text_layer, s_uptime_buffer);

    // Increment s_uptime
    s_uptime++;

	// we don't want to continually send the data points
	// so try to dump the data every two minutes or 120 ticks.
	if (s_uptime % 120 == 0) {
	  if (is_dumping) { // is currently dumping data, and 2 minutes up, reinitialize the log and force storing on the watch
		is_dumping = false;
		init_dlog();
	  } else { // dump to the phone
		is_dumping = true;
		data_logging_finish(accel_log);
	  }
	}
}

void _subscribe_activity(int label) {
  tick_timer_service_subscribe(SECOND_UNIT, &handle_tick);
  accel_data_service_subscribe(SAMPLE_BATCH, &accel_data_handler);
  current_label = label;
  is_recording = true;
  init_dlog(); // create new logging instance since we can't log to same instance after calling finish
}

// This is to receive message back from phone
static void inbox_received_callback(DictionaryIterator *iter, void *context)
{
    APP_LOG(APP_LOG_LEVEL_DEBUG, "==> receive message back");
    Tuple *t = dict_read_first(iter);
    if(t)
    {
        vibes_short_pulse();
		if (t->key == SAVE_ACTIVITY) {
		  APP_LOG(APP_LOG_LEVEL_DEBUG, "write persistent %s", (const char*)t->value->data);
		  persist_write_string(SAVE_ACTIVITY, (const char*)t->value->data);
		  num_save_items += 1;
		  layer_mark_dirty(menu_layer_get_layer(menu_layer));
		  menu_layer_reload_data(menu_layer);
		}
		
		APP_LOG(APP_LOG_LEVEL_DEBUG, "receiving %"PRIu32" %s\n", t->key, (const char*) t->value->data);
		// refresh the view 
		is_received_activity = true;
		_subscribe_activity(2); // Customized Activity Label is 2
		send_signal(START_SIGNAL);
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}


// This function is to handle when the middle button is pressed
void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  // if already recording, meanning the user wants to stop recording
  if (is_recording) {
	tick_timer_service_unsubscribe(); // stop the call back
	accel_data_service_unsubscribe();
	snprintf(s_uptime_buffer, sizeof(s_uptime_buffer), TIME_FORMAT_IDLE,0, 0 , 0); // reinitialize the time
	text_layer_set_text(text_layer, s_uptime_buffer); // set the timer
	is_recording = false;

	data_logging_finish(accel_log); // finishing logging that activit

	// send stop signal as APP_MESSAGE to prevent the loggin app to be full
	send_signal(STOP_SIGNAL);

	APP_LOG(APP_LOG_LEVEL_DEBUG, "==> finish logging session");
	is_received_activity = false; // reset the state of receive data from phone
  } else {	// otherwise, start subscribing it again
	 if (cell_index->row == 2) {
	   APP_LOG(APP_LOG_LEVEL_DEBUG, "==> before sending signal");
		if (!is_received_activity) {
		  text_layer_set_text(text_layer, "Please Select Activity on Your Phone");
		  return;
		}
	 }
	 switch(cell_index->section) {
	 case 0:
	   _subscribe_activity(cell_index->row);
	   break;
	 case 1:
	   _subscribe_activity(SAVE_ACTIVITY);
	 }
	 send_signal(START_SIGNAL);
  }
  s_uptime = 0;
  is_dumping = false;
}

void menu_layer_init(GRect bounds) {
  initialize_icon();

   // get the menu layer
  menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
	.get_header_height = menu_get_header_height_callback,
	.draw_header = menu_draw_header_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });

  menu_layer_set_click_config_onto_window(menu_layer, window);
}

void text_layer_init(GRect bounds) {
  // set the text layer for the phone
  // this including the timer and the state
  text_layer = text_layer_create(bounds);
  text_layer_set_font(text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(text_layer, GTextAlignmentCenter);
  snprintf(s_uptime_buffer, sizeof(s_uptime_buffer), TIME_FORMAT_IDLE,0, 0 , 0);
  text_layer_set_text(text_layer, s_uptime_buffer);
  text_layer_set_overflow_mode(text_layer, GTextOverflowModeWordWrap);
}

void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_frame(window_layer);

  menu_layer_init(GRect(0,0, bounds.size.w, bounds.size.h/2));
  text_layer_init(GRect(0,bounds.size.h/2, bounds.size.w, bounds.size.h/2));
  layer_add_child(window_layer, text_layer_get_layer(text_layer));
  layer_add_child(window_layer, menu_layer_get_layer(menu_layer));
}

void window_unload(Window *window) {
  // Destroy the menu layer
  menu_layer_destroy(menu_layer);

  // Destroy the text layer
  text_layer_destroy(text_layer);

  // unsubscribe timer service
  tick_timer_service_unsubscribe();

  // accel data deinit
  accel_data_service_unsubscribe();

  // Cleanup the menu icons
  for (int i = 0; i < NUM_MENU_ICONS; i++) {
    gbitmap_destroy(menu_icons[i]);
  }
}

void init_message() {
  // Register callbacks
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
}

int main(void) {
  window = window_create();

  // Setup the window handlers
  window_set_window_handlers(window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });

  window_stack_push(window, true );

  startTime = time(NULL);

  init_message();

  app_event_loop();

  window_destroy(window);
}
