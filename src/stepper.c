// Handling of stepper drivers.
//
// Copyright (C) 2016-2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_*
#include "basecmd.h" // oid_alloc
#include "board/gpio.h" // gpio_out_write
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_is_before
#include "command.h" // DECL_COMMAND
#include "sched.h" // struct timer
#include "stepper.h" // command_config_stepper

#include <math.h> // sqrt

#define abs_clamp(x, t) (((x) > (t)) ? (t) : (((x) < (-t)) ? (-t) : (x)))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define abs(x) (((x) < 0) ? (-x) : (x))

DECL_CONSTANT("STEP_DELAY", CONFIG_STEP_DELAY);


/****************************************************************
 * Steppers
 ****************************************************************/
struct stepper_move {
    uint32_t interval;
    int16_t add;
    uint16_t count;
    struct stepper_move *next;
    uint8_t flags;
};

enum { MF_DIR=1<<0 };

struct rt_data {
    struct timer control_timer;
    struct timer step_timer;
    struct i2c_config i2c_config;

    uint8_t slowdown, dir_save, current_dir;

    uint16_t control_freq, input_cycle;
    uint32_t control_period;
    int32_t input_factor;
    uint32_t min_freq, max_freq, max_acc;

    int32_t max_delta_freq, freq_limiter;
    int32_t current_speed, target_speed;
    uint32_t current_period;

    int32_t count, min_pos, max_pos;
    uint16_t cycle_count;
    uint32_t last_step;

    uint8_t slowdown_pending;
    uint32_t slowdown_clock;
};

struct stepper {
    struct timer time;
    uint32_t interval;
    int16_t add;
#if CONFIG_STEP_DELAY <= 0
    uint_fast16_t count;
#define next_step_time time.waketime
#else
    uint32_t count;
    uint32_t next_step_time;
#endif
    struct gpio_out step_pin, dir_pin;
    uint32_t position;
    struct stepper_move *first, **plast;
    uint32_t min_stop_interval;
    // gcc (pre v6) does better optimization when uint8_t are bitfields
    uint8_t flags : 8;

    uint8_t mode, toggle_pending;
    struct timer toggle_mode_timer;
    struct rt_data rt;
};

enum { POSITION_BIAS=0x40000000 };

enum {
    SF_LAST_DIR=1<<0, SF_NEXT_DIR=1<<1, SF_INVERT_STEP=1<<2, SF_HAVE_ADD=1<<3,
    SF_LAST_RESET=1<<4, SF_NO_NEXT_CHECK=1<<5, SF_NEED_RESET=1<<6
};

enum {
    RT_STEP=1<<0, RT_CHANGE_DIR=1<<1, RT_TIMER_STOP=1<<2
};

enum {
    HOST_MODE=0, REALTIME_MODE=1
};

static struct task_wake rt_control_wake;
static struct task_wake toggle_mode_wake;

// Setup a stepper for the next move in its queue
static uint_fast8_t
stepper_load_next(struct stepper *s, uint32_t min_next_time)
{
    struct stepper_move *m = s->first;
    if (!m) {
        // There is no next move - the queue is empty
        if (s->interval - s->add < s->min_stop_interval
            && !(s->flags & SF_NO_NEXT_CHECK))
            shutdown("No next step");
        s->count = 0;
        return SF_DONE;
    }

    // Load next 'struct stepper_move' into 'struct stepper'
    s->next_step_time += m->interval;
    s->add = m->add;
    s->interval = m->interval + m->add;
    if (CONFIG_STEP_DELAY <= 0) {
        if (CONFIG_MACH_AVR)
            // On AVR see if the add can be optimized away
            s->flags = m->add ? s->flags|SF_HAVE_ADD : s->flags & ~SF_HAVE_ADD;
        s->count = m->count;
    } else {
        // On faster mcus, it is necessary to schedule unstep events
        // and so there are twice as many events.  Also check that the
        // next step event isn't too close to the last unstep.
        if (unlikely(timer_is_before(s->next_step_time, min_next_time))) {
            if ((int32_t)(s->next_step_time - min_next_time)
                < (int32_t)(-timer_from_us(1000)))
                shutdown("Stepper too far in past");
            s->time.waketime = min_next_time;
        } else {
            s->time.waketime = s->next_step_time;
        }
        s->count = (uint32_t)m->count * 2;
    }
    // Add all steps to s->position (stepper_get_position() can calc mid-move)
    if (m->flags & MF_DIR) {
        s->position = -s->position + m->count;
        if(s->mode == REALTIME_MODE) {
            s->rt.dir_save = !s->rt.dir_save;
        }
        else {
            gpio_out_toggle_noirq(s->dir_pin);
        }
    } else {
        s->position += m->count;
    }

    s->first = m->next;
    move_free(m);
    return SF_RESCHEDULE;
}

// AVR optimized step function
static uint_fast8_t
stepper_event_avr(struct stepper *s)
{
    gpio_out_toggle_noirq(s->step_pin);
    uint_fast16_t count = s->count - 1;
    if (likely(count)) {
        s->count = count;
        s->time.waketime += s->interval;
        gpio_out_toggle_noirq(s->step_pin);
        if (s->flags & SF_HAVE_ADD)
            s->interval += s->add;
        return SF_RESCHEDULE;
    }
    uint_fast8_t ret = stepper_load_next(s, 0);
    gpio_out_toggle_noirq(s->step_pin);
    return ret;
}

// Optimized step function for stepping and unstepping in same function
static uint_fast8_t
stepper_event_nodelay(struct stepper *s)
{
    gpio_out_toggle_noirq(s->step_pin);
    uint_fast16_t count = s->count - 1;
    if (likely(count)) {
        s->count = count;
        s->time.waketime += s->interval;
        s->interval += s->add;
        gpio_out_toggle_noirq(s->step_pin);
        return SF_RESCHEDULE;
    }
    uint_fast8_t ret = stepper_load_next(s, 0);
    gpio_out_toggle_noirq(s->step_pin);
    return ret;
}

// Timer callback - step the given stepper.
uint_fast8_t
stepper_event(struct timer *t)
{
    struct stepper *s = container_of(t, struct stepper, time);
    if (CONFIG_STEP_DELAY <= 0 && CONFIG_MACH_AVR)
        return stepper_event_avr(s);
    if (CONFIG_STEP_DELAY <= 0)
        return stepper_event_nodelay(s);

    // Normal step code - schedule the unstep event
    if (!CONFIG_HAVE_STRICT_TIMING)
        gpio_out_toggle_noirq(s->step_pin);
    uint32_t step_delay = timer_from_us(CONFIG_STEP_DELAY);
    uint32_t min_next_time = timer_read_time() + step_delay;
    if (CONFIG_HAVE_STRICT_TIMING)
        // Toggling gpio after reading the time is a micro-optimization
        gpio_out_toggle_noirq(s->step_pin);
    s->count--;
    if (likely(s->count & 1))
        // Schedule unstep event
        goto reschedule_min;
    if (likely(s->count)) {
        s->next_step_time += s->interval;
        s->interval += s->add;
        if (unlikely(timer_is_before(s->next_step_time, min_next_time)))
            // The next step event is too close - push it back
            goto reschedule_min;
        s->time.waketime = s->next_step_time;
        return SF_RESCHEDULE;
    }
    return stepper_load_next(s, min_next_time);
reschedule_min:
    s->time.waketime = min_next_time;
    return SF_RESCHEDULE;
}

void
command_config_stepper(uint32_t *args)
{
    struct stepper *s = oid_alloc(args[0], command_config_stepper, sizeof(*s));
    if (!CONFIG_INLINE_STEPPER_HACK)
        s->time.func = stepper_event;
    s->flags = args[4] ? SF_INVERT_STEP : 0;
    s->step_pin = gpio_out_setup(args[1], s->flags & SF_INVERT_STEP);
    s->dir_pin = gpio_out_setup(args[2], 0);
    s->min_stop_interval = args[3];
    s->position = -POSITION_BIAS;
    s->mode = HOST_MODE;
    s->toggle_pending = 0;
    move_request_size(sizeof(struct stepper_move));
}
DECL_COMMAND(command_config_stepper,
             "config_stepper oid=%c step_pin=%c dir_pin=%c"
             " min_stop_interval=%u invert_step=%c");

void
command_config_stepper_rt_mode(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    s->rt.control_freq = args[1];
    s->rt.input_cycle  = args[2];
    s->rt.input_factor = args[3];
    s->rt.max_freq     = args[4];
    s->rt.max_acc      = args[5];

    s->rt.control_period = CONFIG_CLOCK_FREQ / s->rt.control_freq;
    s->rt.max_delta_freq = s->rt.max_acc / s->rt.control_freq;
    // to ensure start off is possible, min_freq is never above max_delta_freq
    s->rt.min_freq = min(100, s->rt.max_delta_freq);
    s->rt.slowdown_pending = 0;

    s->rt.i2c_config = i2c_setup(0, 400000, 0x48);
    uint8_t ads1015_conf[3] = {0x01, 0x42, 0x63};
    i2c_write(s->rt.i2c_config, 3, ads1015_conf);
}
DECL_COMMAND(command_config_stepper_rt_mode,
             "config_stepper_rt_mode oid=%c control_freq=%hu input_cycle=%hu"
             " input_factor=%i max_freq=%u max_acc=%u");

// Return the 'struct stepper' for a given stepper oid
struct stepper *
stepper_oid_lookup(uint8_t oid)
{
    return oid_lookup(oid, command_config_stepper);
}

// Schedule a set of steps with a given timing
void
command_queue_step(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    struct stepper_move *m = move_alloc();
    m->interval = args[1];
    m->count = args[2];
    if (!m->count)
        shutdown("Invalid count parameter");
    m->add = args[3];
    m->next = NULL;
    m->flags = 0;

    irq_disable();
    uint8_t flags = s->flags;
    if (!!(flags & SF_LAST_DIR) != !!(flags & SF_NEXT_DIR)) {
        flags ^= SF_LAST_DIR;
        m->flags |= MF_DIR;
    }
    flags &= ~SF_NO_NEXT_CHECK;
    if (m->count == 1 && (m->flags || flags & SF_LAST_RESET))
        // count=1 moves after a reset or dir change can have small intervals
        flags |= SF_NO_NEXT_CHECK;
    flags &= ~SF_LAST_RESET;
    if (s->count) {
        s->flags = flags;
        if (s->first)
            *s->plast = m;
        else
            s->first = m;
        s->plast = &m->next;
    } else if (flags & SF_NEED_RESET) {
        move_free(m);
    } else {
        s->flags = flags;
        s->first = m;
        stepper_load_next(s, s->next_step_time + m->interval);
        sched_add_timer(&s->time);
    }
    irq_enable();
}
DECL_COMMAND(command_queue_step,
             "queue_step oid=%c interval=%u count=%hu add=%hi");

// Set the direction of the next queued step
void
command_set_next_step_dir(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    uint8_t nextdir = args[1] ? SF_NEXT_DIR : 0;
    irq_disable();
    s->flags = (s->flags & ~SF_NEXT_DIR) | nextdir;
    irq_enable();
}
DECL_COMMAND(command_set_next_step_dir, "set_next_step_dir oid=%c dir=%c");

// Set an absolute time that the next step will be relative to
void
command_reset_step_clock(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    uint32_t waketime = args[1];
    irq_disable();
    if (s->count)
        shutdown("Can't reset time when stepper active");
    s->next_step_time = waketime;
    s->flags = (s->flags & ~SF_NEED_RESET) | SF_LAST_RESET;
    irq_enable();
}
DECL_COMMAND(command_reset_step_clock, "reset_step_clock oid=%c clock=%u");

// Return the current stepper position.  Caller must disable irqs.
static uint32_t
stepper_get_position(struct stepper *s)
{
    uint32_t position = s->position;
    // If stepper is mid-move, subtract out steps not yet taken
    if (CONFIG_STEP_DELAY <= 0)
        position -= s->count;
    else
        position -= s->count / 2;
    // The top bit of s->position is an optimized reverse direction flag
    if (position & 0x80000000)
        return -position;
    return position;
}

// Report the current position of the stepper
void
command_stepper_get_position(uint32_t *args)
{
    uint8_t oid = args[0];
    struct stepper *s = stepper_oid_lookup(oid);
    irq_disable();
    uint32_t position = stepper_get_position(s);
    irq_enable();
    sendf("stepper_position oid=%c pos=%i", oid, position - POSITION_BIAS);
}
DECL_COMMAND(command_stepper_get_position, "stepper_get_position oid=%c");

static int32_t low_pass = 0;
int32_t get_error(struct stepper *s)
{
    uint8_t reading[2];
    i2c_read(s->rt.i2c_config, 1, 0x00, 2, reading);
    int32_t val = (reading[0] * 256 + reading[1]) >> 4;

    // compute target speed
    val = (val - 1024);

    low_pass = (low_pass + val) / 2;
    return low_pass;
}

void rt_control_run(struct stepper *s)
{
    // get target speed every rt.input_cycle cycles
    if(s->rt.cycle_count == 0) {
        int32_t error = get_error(s);
        sendf("stepper_rt_log pos=%i error=%i", s->rt.count, error);
        s->rt.target_speed = error * s->rt.input_factor;
        s->rt.target_speed = abs_clamp(
                             s->rt.target_speed, (int32_t)s->rt.max_freq);
    }

    // apply position based limiter (to avoid stepper max position overrun)
    int32_t dist_to_min = max(0, s->rt.count - (s->rt.min_pos + 1));
    int32_t dist_to_max = max(0, (s->rt.max_pos - 1) - s->rt.count);
    uint32_t steps_to_stop = pow(s->rt.max_freq, 2) / (2 * s->rt.max_acc)
                             + 2 * s->rt.max_freq / s->rt.control_freq;

    if(dist_to_min <= steps_to_stop)
    {
        int32_t limit = sqrt((float)s->rt.max_acc * dist_to_min);
        limit = max(0, limit);
        s->rt.target_speed = max(s->rt.target_speed, -limit);
    }
    if(dist_to_max <= steps_to_stop)
    {
        int32_t limit = sqrt((float)s->rt.max_acc * dist_to_max);
        limit = max(0, limit);
        s->rt.target_speed = min(s->rt.target_speed, limit);
    }

    // time based limiter (for slowdown)
    if(s->rt.slowdown) {
        if(s->rt.freq_limiter < s->rt.max_delta_freq) {
            sched_del_timer(&s->rt.step_timer);
            sched_del_timer(&s->rt.control_timer);
            if(s->rt.current_dir != s->rt.dir_save) {
                gpio_out_toggle_noirq(s->dir_pin);
            }
            if(s->position & 0x80000000) {
                s->position = -(s->rt.count + POSITION_BIAS) | 0x80000000;
            }
            else {
                s->position = s->rt.count + POSITION_BIAS;
            }
            s->mode = HOST_MODE;
            return;
        }
        s->rt.freq_limiter -= s->rt.max_delta_freq;
        s->rt.target_speed = abs_clamp(s->rt.target_speed, s->rt.freq_limiter);
    }

    // compute new reachable speed according to acceleration
    int32_t delta = s->rt.target_speed - s->rt.current_speed;
    s->rt.current_speed += abs_clamp(delta, s->rt.max_delta_freq);

    // speed is either above min_speed, either null
    if (abs(s->rt.current_speed) < s->rt.min_freq) {
        s->rt.current_speed = 0;
    }

    // store previous direction
    uint8_t prev_dir = s->rt.current_speed < 0;

    // compute period according to speed, period of zero means no speed
    if(abs(s->rt.current_speed) > 0) {
        s->rt.current_period = CONFIG_CLOCK_FREQ / abs(s->rt.current_speed);
    }
    else {
        s->rt.current_period = 0;
    }

    irq_disable();
    // possibly apply direction change
    if (prev_dir != s->rt.current_dir) {
        gpio_out_toggle_noirq(s->dir_pin);
        s->rt.current_dir = !s->rt.current_dir;
    }
    irq_enable();

    s->rt.cycle_count = (s->rt.cycle_count + 1) % s->rt.input_cycle;
}

static uint_fast8_t
rt_step_event(struct timer *t)
{
    struct rt_data *rt = container_of(t, struct rt_data, step_timer);
    struct stepper *s = container_of(rt, struct stepper, rt);
    if(s->rt.current_period == 0) {
        t->waketime += s->rt.control_period;
    }
    else {
        gpio_out_toggle_noirq(s->step_pin);
        s->rt.last_step = t->waketime;
        t->waketime += s->rt.current_period;
        s->rt.count += s->rt.current_dir ? -1 : 1;
        gpio_out_toggle_noirq(s->step_pin);
    }
    return SF_RESCHEDULE;
}

static uint_fast8_t
rt_control_event(struct timer *t)
{
    struct rt_data *rt = container_of(t, struct rt_data, control_timer);
    struct stepper *s = container_of(rt, struct stepper, rt);
    t->waketime += s->rt.control_period;
    sched_wake_task(&rt_control_wake);
    return SF_RESCHEDULE;
}

uint_fast8_t
toggle_mode_event(struct timer *t)
{
    struct stepper *s = container_of(t, struct stepper, toggle_mode_timer);
    s->toggle_pending = 1;
    sched_wake_task(&toggle_mode_wake);
    t->func = NULL;
    return SF_DONE;
}
void

schedule_slowdown(struct stepper *s, uint32_t clock)
{
    // call transition in advance to slowdown
    uint32_t slowdown_time = s->rt.control_period * (s->rt.max_freq / s->rt.max_delta_freq);

    if(timer_is_before(clock - slowdown_time, timer_read_time())) {
        uint32_t rest_time = clock - timer_read_time();
        s->rt.freq_limiter = s->rt.max_delta_freq * rest_time
                             / s->rt.control_period;
        s->rt.slowdown = 1;
    }
    else {
        sched_del_timer(&s->toggle_mode_timer);
        s->toggle_mode_timer.waketime = clock - slowdown_time;
        s->toggle_mode_timer.func = toggle_mode_event;
        sched_add_timer(&s->toggle_mode_timer);
    }
}

void host_to_realtime_mode(struct stepper *s)
{
    if(!(s->flags & SF_LAST_DIR)) {
        gpio_out_toggle_noirq(s->dir_pin);
        s->rt.dir_save = 1;
    }
    else {
        s->rt.dir_save = 0;
    }
    s->rt.count = stepper_get_position(s) - POSITION_BIAS;
    s->rt.current_dir    = 0;
    s->rt.slowdown       = 0;
    s->rt.current_period = 0;
    s->rt.current_speed  = 0;
    s->rt.cycle_count    = 0;
    s->rt.last_step      = 0;
    s->rt.cycle_count    = 0;

    s->rt.control_timer.func = rt_control_event;
    s->rt.control_timer.waketime = timer_read_time() + CONFIG_CLOCK_FREQ / 10000;
    sched_add_timer(&s->rt.control_timer);

    s->rt.step_timer.func = rt_step_event;
    s->rt.step_timer.waketime = timer_read_time() + CONFIG_CLOCK_FREQ / 5000;
    sched_add_timer(&s->rt.step_timer);

    s->mode = REALTIME_MODE;

    if(s->rt.slowdown_pending) {
        schedule_slowdown(s, s->rt.slowdown_clock);
        s->rt.slowdown_pending = 0;
    }
}

void realtime_to_host_mode(struct stepper *s)
{
    s->rt.freq_limiter = s->rt.max_freq;
    s->rt.slowdown = 1;
    // realtime control will switch mode after slowdown
}

// Set stepper host control mode
void
command_set_host_mode(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    if(s->mode == REALTIME_MODE) {
        schedule_slowdown(s, args[1]);
    }
    else {
        s->rt.slowdown_pending = 1;
        s->rt.slowdown_clock = args[1];
    }
}
DECL_COMMAND(command_set_host_mode, "set_host_mode oid=%c clock=%u");

// Set stepper realtime control mode
void
command_set_realtime_mode(uint32_t *args)
{
    struct stepper *s = stepper_oid_lookup(args[0]);
    if(s->mode == HOST_MODE && s->toggle_mode_timer.func == NULL) {
        sched_del_timer(&s->toggle_mode_timer);
        s->toggle_mode_timer.waketime = args[1];
        s->rt.min_pos = args[2];
        s->rt.max_pos = args[3];
        s->toggle_mode_timer.func = toggle_mode_event;
        sched_add_timer(&s->toggle_mode_timer);
    }
    else {
        shutdown("Prevent stepper realtime mode enable twice.");
    }
}
DECL_COMMAND(command_set_realtime_mode,
             "set_realtime_mode oid=%c clock=%u min_pos=%i max_pos=%i");

// Stop all moves for a given stepper (used in end stop homing).  IRQs
// must be off.
void
stepper_stop(struct stepper *s)
{
    sched_del_timer(&s->time);
    s->next_step_time = 0;
    s->position = -stepper_get_position(s);
    s->count = 0;
    s->flags = (s->flags & SF_INVERT_STEP) | SF_NEED_RESET;
    gpio_out_write(s->dir_pin, 0);
    gpio_out_write(s->step_pin, s->flags & SF_INVERT_STEP);
    while (s->first) {
        struct stepper_move *next = s->first->next;
        move_free(s->first);
        s->first = next;
    }
}

void
toggle_mode_task(void)
{
    if (!sched_check_wake(&toggle_mode_wake))
        return;

    uint8_t i;
    struct stepper *s;
    foreach_oid(i, s, command_config_stepper) {
        if(s->toggle_pending) {
            if(s->mode == HOST_MODE) {
                host_to_realtime_mode(s);
            }
            else {
                realtime_to_host_mode(s);
            }
            s->toggle_pending = 0;
        }
    }
}
DECL_TASK(toggle_mode_task);

void
rt_control_task(void)
{
    if (!sched_check_wake(&rt_control_wake))
        return;

    uint8_t i;
    struct stepper *s;
    foreach_oid(i, s, command_config_stepper) {
        if(s->mode == REALTIME_MODE)
            rt_control_run(s);
    }
}
DECL_TASK(rt_control_task);

void
stepper_shutdown(void)
{
    uint8_t i;
    struct stepper *s;
    foreach_oid(i, s, command_config_stepper) {
        s->first = NULL;
        stepper_stop(s);
    }
}
DECL_SHUTDOWN(stepper_shutdown);
