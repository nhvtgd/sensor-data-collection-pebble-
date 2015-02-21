#include <pebble.h>
#include <inttypes.h>

#define NUM_MENU_ICONS 3
#define NUM_ITEMS 3
#define NUM_SECTIONS 1
#define SAMPLE_BATCH 18

static Window *window;

static MenuLayer *menu_layer;

static GBitmap *menu_icons[NUM_MENU_ICONS];

static TextLayer *text_layer;

static const char* const activity_list[] = {"Running", "Walking", "Custom Activity"};

static const char* const TIME_FORMAT_RECORDING =  "%02d:%02d:%02d Recording";

static const char* const TIME_FORMAT_IDLE =  "%02d:%02d:%02d Stop";
// startting time for the counter
static int s_uptime = 0;

static char s_uptime_buffer[48];

bool is_start;

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

void send_signal(uint8_t key, uint8_t cmd) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);

  Tuplet value = TupletInteger(key, cmd);
  dict_write_tuplet(iter, &value);

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
	APP_LOG(APP_LOG_LEVEL_DEBUG, getStatus(r));
  }
}

void initialize_icon() {
  int num_menu_icons = 0;
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_RUNNING);
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_WALKING);
  menu_icons[num_menu_icons++] = gbitmap_create_with_resource(RESOURCE_ID_ADD_ACTIVITY);
}

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return NUM_SECTIONS;
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return NUM_ITEMS;
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
	// so try to dump the data every two minutes.
	
	if (minutes != 0 && minutes % 2 == 0) {
		is_dumping = true;
		data_logging_finish(accel_log);
	} else {
	    is_dumping = false;
		init_dlog();
		}
}

// This is to receive message back from phone
static void in_received_handler(DictionaryIterator *iter, void *context)
{
  APP_LOG(APP_LOG_LEVEL_DEBUG, "==> receive message back");
    Tuple *t = dict_read_first(iter);
    if(t)
    {
        vibes_short_pulse();
		is_received_activity = true;
		tick_timer_service_subscribe(SECOND_UNIT, &handle_tick);
		accel_data_service_subscribe(SAMPLE_BATCH, &accel_data_handler);
		current_label = 2;
		is_start = true;
		init_dlog(); // create new logging instance since we can't log to same instance after calling finish
    }
}

// This function is to handle when the middle button is pressed
void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  // if already started, meanning the user wants to stop recording
  if (is_start) {
	tick_timer_service_unsubscribe(); // stop the call back
	accel_data_service_unsubscribe();
	snprintf(s_uptime_buffer, sizeof(s_uptime_buffer), TIME_FORMAT_IDLE,0, 0 , 0); // reinitialize the time
	text_layer_set_text(text_layer, s_uptime_buffer); // set the timer
	is_start = false;
	data_logging_finish(accel_log); // finishing logging that activit
	APP_LOG(APP_LOG_LEVEL_DEBUG, "==> finish logging session");
	is_received_activity = false; // reset the state of receive data from phone
  } else {	// otherwise, start subscribing it again
	 if (cell_index->row == 2) {
		APP_LOG(APP_LOG_LEVEL_DEBUG, "==> before sending signal");
		send_signal(0,1);
		if (!is_received_activity) {
		  text_layer_set_text(text_layer, "Please Select Activity on Your Phone");
		  return;
		}
	 }
	tick_timer_service_subscribe(SECOND_UNIT, &handle_tick);
	accel_data_service_subscribe(SAMPLE_BATCH, &accel_data_handler);
	current_label = cell_index->row;
	is_start = true;
	init_dlog(); // create new logging instance since we can't log to same instance after calling finish
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

int main(void) {
  window = window_create();

  // Setup the window handlers
  window_set_window_handlers(window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });

  window_stack_push(window, true );

  // reigster app message to send message to phone.
  app_message_register_inbox_received(in_received_handler);
  app_message_open(512, 512);

  app_event_loop();

  window_destroy(window);
}
