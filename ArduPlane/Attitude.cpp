#include "Plane.h"
#include <AP_HAL/AP_HAL.h>

extern const AP_HAL::HAL& hal;

/*
  get a speed scaling number for control surfaces. This is applied to
  PIDs to change the scaling of the PID with speed. At high speed we
  move the surfaces less, and at low speeds we move them more.
 */
float Plane::get_speed_scaler(void)
{
    float aspeed, speed_scaler;
    if (ahrs.airspeed_estimate(&aspeed)) {
        if (aspeed > auto_state.highest_airspeed) {
            auto_state.highest_airspeed = aspeed;
        }
        if (aspeed > 0.0001f) {
            speed_scaler = g.scaling_speed / aspeed;
        } else {
            speed_scaler = 2.0;
        }
        speed_scaler = constrain_float(speed_scaler, 0.5f, 2.0f);
    } else {
        if (channel_throttle->get_servo_out() > 0) {
            speed_scaler = 0.5f + ((float)THROTTLE_CRUISE / channel_throttle->get_servo_out() / 2.0f);                 // First order taylor expansion of square root
            // Should maybe be to the 2/7 power, but we aren't going to implement that...
        }else{
            speed_scaler = 1.67f;
        }
        // This case is constrained tighter as we don't have real speed info
        speed_scaler = constrain_float(speed_scaler, 0.6f, 1.67f);
    }
    return speed_scaler;
}

/*
  return true if the current settings and mode should allow for stick mixing
 */
bool Plane::stick_mixing_enabled(void)
{
    if (auto_throttle_mode && auto_navigation_mode) {
        // we're in an auto mode. Check the stick mixing flag
        if (g.stick_mixing != STICK_MIXING_DISABLED &&
            geofence_stickmixing() &&
            failsafe.state == FAILSAFE_NONE &&
            !rc_failsafe_active()) {
            // we're in an auto mode, and haven't triggered failsafe
            return true;
        } else {
            return false;
        }
    }

    if (failsafe.ch3_failsafe && g.short_fs_action == 2) {
        // don't do stick mixing in FBWA glide mode
        return false;
    }

    // non-auto mode. Always do stick mixing
    return true;
}


/*
  this is the main roll stabilization function. It takes the
  previously set nav_roll calculates roll servo_out to try to
  stabilize the plane at the given roll
 */
void Plane::stabilize_roll(float speed_scaler)
{
    if (fly_inverted()) {
        // we want to fly upside down. We need to cope with wrap of
        // the roll_sensor interfering with wrap of nav_roll, which
        // would really confuse the PID code. The easiest way to
        // handle this is to ensure both go in the same direction from
        // zero
        nav_roll_cd += 18000;
        if (ahrs.roll_sensor < 0) nav_roll_cd -= 36000;
    }

    bool disable_integrator = false;
    if (control_mode == STABILIZE && channel_roll->get_control_in() != 0) {
        disable_integrator = true;
    }
    channel_roll->set_servo_out(rollController.get_servo_out(nav_roll_cd - ahrs.roll_sensor, 
                                                           speed_scaler, 
                                                           disable_integrator));
}

/*
  this is the main pitch stabilization function. It takes the
  previously set nav_pitch and calculates servo_out values to try to
  stabilize the plane at the given attitude.
 */
void Plane::stabilize_pitch(float speed_scaler)
{
    int8_t force_elevator = takeoff_tail_hold();
    if (force_elevator != 0) {
        // we are holding the tail down during takeoff. Just convert
        // from a percentage to a -4500..4500 centidegree angle
        channel_pitch->set_servo_out(45*force_elevator);
        return;
    }
    int32_t demanded_pitch = nav_pitch_cd + g.pitch_trim_cd + channel_throttle->get_servo_out() * g.kff_throttle_to_pitch;
    bool disable_integrator = false;
    if (control_mode == STABILIZE && channel_pitch->get_control_in() != 0) {
        disable_integrator = true;
    }
    channel_pitch->set_servo_out(pitchController.get_servo_out(demanded_pitch - ahrs.pitch_sensor, 
                                                             speed_scaler, 
                                                             disable_integrator));
}

/*
  perform stick mixing on one channel
  This type of stick mixing reduces the influence of the auto
  controller as it increases the influence of the users stick input,
  allowing the user full deflection if needed
 */
void Plane::stick_mix_channel(RC_Channel *channel, int16_t &servo_out)
{
    float ch_inf;
        
    ch_inf = (float)channel->get_radio_in() - (float)channel->get_radio_trim();
    ch_inf = fabsf(ch_inf);
    ch_inf = MIN(ch_inf, 400.0f);
    ch_inf = ((400.0f - ch_inf) / 400.0f);
    servo_out *= ch_inf;
    servo_out += channel->pwm_to_angle();
}

/*
  One argument version for when the servo out in the rc channel 
  is the target
 */
void Plane::stick_mix_channel(RC_Channel * channel)
{
   int16_t servo_out = channel->get_servo_out();
   stick_mix_channel(channel,servo_out);
   channel->set_servo_out(servo_out);
}

/*
  this gives the user control of the aircraft in stabilization modes
 */
void Plane::stabilize_stick_mixing_direct()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == QLAND ||
        control_mode == QRTL ||
        control_mode == TRAINING) {
        return;
    }
    stick_mix_channel(channel_roll);
    stick_mix_channel(channel_pitch);
}

/*
  this gives the user control of the aircraft in stabilization modes
  using FBW style controls
 */
void Plane::stabilize_stick_mixing_fbw()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == QLAND ||
        control_mode == QRTL ||
        control_mode == TRAINING ||
        (control_mode == AUTO && g.auto_fbw_steer == 42)) {
        return;
    }
    // do FBW style stick mixing. We don't treat it linearly
    // however. For inputs up to half the maximum, we use linear
    // addition to the nav_roll and nav_pitch. Above that it goes
    // non-linear and ends up as 2x the maximum, to ensure that
    // the user can direct the plane in any direction with stick
    // mixing.
    float roll_input = channel_roll->norm_input();
    if (roll_input > 0.5f) {
        roll_input = (3*roll_input - 1);
    } else if (roll_input < -0.5f) {
        roll_input = (3*roll_input + 1);
    }
    nav_roll_cd += roll_input * roll_limit_cd;
    nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit_cd, roll_limit_cd);
    
    float pitch_input = channel_pitch->norm_input();
    if (pitch_input > 0.5f) {
        pitch_input = (3*pitch_input - 1);
    } else if (pitch_input < -0.5f) {
        pitch_input = (3*pitch_input + 1);
    }
    if (fly_inverted()) {
        pitch_input = -pitch_input;
    }
    if (pitch_input > 0) {
        nav_pitch_cd += pitch_input * aparm.pitch_limit_max_cd;
    } else {
        nav_pitch_cd += -(pitch_input * pitch_limit_min_cd);
    }
    nav_pitch_cd = constrain_int32(nav_pitch_cd, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}


/*
  stabilize the yaw axis. There are 3 modes of operation:

    - hold a specific heading with ground steering
    - rate controlled with ground steering
    - yaw control for coordinated flight    
 */
void Plane::stabilize_yaw(float speed_scaler)
{
    if (control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) {
        // in land final setup for ground steering
        steering_control.ground_steering = true;
    } else {
        // otherwise use ground steering when no input control and we
        // are below the GROUND_STEER_ALT
        steering_control.ground_steering = (channel_roll->get_control_in() == 0 && 
                                            fabsf(relative_altitude()) < g.ground_steer_alt);
        if (control_mode == AUTO &&
                (flight_stage == AP_SpdHgtControl::FLIGHT_LAND_APPROACH ||
                flight_stage == AP_SpdHgtControl::FLIGHT_LAND_PREFLARE)) {
            // don't use ground steering on landing approach
            steering_control.ground_steering = false;
        }
    }


    /*
      first calculate steering_control.steering for a nose or tail
      wheel.
      We use "course hold" mode for the rudder when either in the
      final stage of landing (when the wings are help level) or when
      in course hold in FBWA mode (when we are below GROUND_STEER_ALT)
     */
    if ((control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) ||
        (steer_state.hold_course_cd != -1 && steering_control.ground_steering)) {
        calc_nav_yaw_course();
    } else if (steering_control.ground_steering) {
        calc_nav_yaw_ground();
    }

    /*
      now calculate steering_control.rudder for the rudder
     */
    calc_nav_yaw_coordinated(speed_scaler);
}


/*
  a special stabilization function for training mode
 */
void Plane::stabilize_training(float speed_scaler)
{
    if (training_manual_roll) {
      if (hal.rcin->read(7-1) >= 1700) {
	  // the user has enabled chirp control test code
	channel_roll->set_servo_out(control_chirp());
	}
      else{
        channel_roll->set_servo_out(channel_roll->get_control_in());
      }
    } else {
        // calculate what is needed to hold
        stabilize_roll(speed_scaler);
        if ((nav_roll_cd > 0 && channel_roll->get_control_in() < channel_roll->get_servo_out()) ||
            (nav_roll_cd < 0 && channel_roll->get_control_in() > channel_roll->get_servo_out())) {
            // allow user to get out of the roll
            channel_roll->set_servo_out(channel_roll->get_control_in());            
        }
    }

    if (training_manual_pitch) {
        channel_pitch->set_servo_out(channel_pitch->get_control_in());
    } else {
        stabilize_pitch(speed_scaler);
        if ((nav_pitch_cd > 0 && channel_pitch->get_control_in() < channel_pitch->get_servo_out()) ||
            (nav_pitch_cd < 0 && channel_pitch->get_control_in() > channel_pitch->get_servo_out())) {
            // allow user to get back to level
            channel_pitch->set_servo_out(channel_pitch->get_control_in());            
        }
    }

    stabilize_yaw(speed_scaler);
}


/*
  this is the ACRO mode stabilization function. It does rate
  stabilization on roll and pitch axes
 */
void Plane::stabilize_acro(float speed_scaler)
{
    float roll_rate = (channel_roll->get_control_in()/4500.0f) * g.acro_roll_rate;
    float pitch_rate = (channel_pitch->get_control_in()/4500.0f) * g.acro_pitch_rate;

    /*
      check for special roll handling near the pitch poles
     */
    if (g.acro_locking && is_zero(roll_rate)) {
        /*
          we have no roll stick input, so we will enter "roll locked"
          mode, and hold the roll we had when the stick was released
         */
        if (!acro_state.locked_roll) {
            acro_state.locked_roll = true;
            acro_state.locked_roll_err = 0;
        } else {
            acro_state.locked_roll_err += ahrs.get_gyro().x * G_Dt;
        }
        int32_t roll_error_cd = -ToDeg(acro_state.locked_roll_err)*100;
        nav_roll_cd = ahrs.roll_sensor + roll_error_cd;
        // try to reduce the integrated angular error to zero. We set
        // 'stabilze' to true, which disables the roll integrator
        channel_roll->set_servo_out(rollController.get_servo_out(roll_error_cd,
                                                                speed_scaler,
                                                                true));
    } else {
        /*
          aileron stick is non-zero, use pure rate control until the
          user releases the stick
         */
        acro_state.locked_roll = false;
        channel_roll->set_servo_out(rollController.get_rate_out(roll_rate,  speed_scaler));
    }

    if (g.acro_locking && is_zero(pitch_rate)) {
        /*
          user has zero pitch stick input, so we lock pitch at the
          point they release the stick
         */
        if (!acro_state.locked_pitch) {
            acro_state.locked_pitch = true;
            acro_state.locked_pitch_cd = ahrs.pitch_sensor;
        }
        // try to hold the locked pitch. Note that we have the pitch
        // integrator enabled, which helps with inverted flight
        nav_pitch_cd = acro_state.locked_pitch_cd;
        channel_pitch->set_servo_out(pitchController.get_servo_out(nav_pitch_cd - ahrs.pitch_sensor,
                                                                  speed_scaler,
                                                                  false));
    } else {
        /*
          user has non-zero pitch input, use a pure rate controller
         */
        acro_state.locked_pitch = false;
        channel_pitch->set_servo_out( pitchController.get_rate_out(pitch_rate, speed_scaler));
    }

    /*
      manual rudder for now
     */
    steering_control.steering = steering_control.rudder = rudder_input;
}

/*
  main stabilization function for all 3 axes
 */
void Plane::stabilize()
{
    if (control_mode == MANUAL) {
        // nothing to do
        return;
    }
    float speed_scaler = get_speed_scaler();

    if (control_mode == TRAINING) {
        stabilize_training(speed_scaler);
    } else if (control_mode == ACRO) {
        stabilize_acro(speed_scaler);
    } else if (control_mode == QSTABILIZE ||
               control_mode == QHOVER ||
               control_mode == QLOITER ||
               control_mode == QLAND ||
               control_mode == QRTL) {
        quadplane.control_run();
    } else {
        if (g.stick_mixing == STICK_MIXING_FBW && control_mode != STABILIZE) {
            stabilize_stick_mixing_fbw();
        }
        stabilize_roll(speed_scaler);
        stabilize_pitch(speed_scaler);
        if (g.stick_mixing == STICK_MIXING_DIRECT || control_mode == STABILIZE) {
            stabilize_stick_mixing_direct();
        }
        stabilize_yaw(speed_scaler);
    }

    /*
      see if we should zero the attitude controller integrators. 
     */
    if (channel_throttle->get_control_in() == 0 &&
        relative_altitude_abs_cm() < 500 && 
        fabsf(barometer.get_climb_rate()) < 0.5f &&
        gps.ground_speed() < 3) {
        // we are low, with no climb rate, and zero throttle, and very
        // low ground speed. Zero the attitude controller
        // integrators. This prevents integrator buildup pre-takeoff.
        rollController.reset_I();
        pitchController.reset_I();
        yawController.reset_I();

        // if moving very slowly also zero the steering integrator
        if (gps.ground_speed() < 1) {
            steerController.reset_I();            
        }
    }
}


void Plane::calc_throttle()
{
    if (aparm.throttle_cruise <= 1) {
        // user has asked for zero throttle - this may be done by a
        // mission which wants to turn off the engine for a parachute
        // landing
        channel_throttle->set_servo_out(0);
        return;
    }

    int32_t commanded_throttle = SpdHgt_Controller->get_throttle_demand();

    // Received an external msg that guides throttle in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_throttle_ms > 0 &&
            millis() - plane.guided_state.last_forced_throttle_ms < 3000) {
        commanded_throttle = plane.guided_state.forced_throttle;
    }

    channel_throttle->set_servo_out(commanded_throttle);
}

/*****************************************
* Calculate desired roll/pitch/yaw angles (in medium freq loop)
*****************************************/

/*
  calculate yaw control for coordinated flight
 */
void Plane::calc_nav_yaw_coordinated(float speed_scaler)
{
    bool disable_integrator = false;
    if (control_mode == STABILIZE && rudder_input != 0) {
        disable_integrator = true;
    }

    int16_t commanded_rudder;

    // Received an external msg that guides yaw in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_rpy_ms.z > 0 &&
            millis() - plane.guided_state.last_forced_rpy_ms.z < 3000) {
        commanded_rudder = plane.guided_state.forced_rpy_cd.z;
    } else {
        commanded_rudder = yawController.get_servo_out(speed_scaler, disable_integrator);

        // add in rudder mixing from roll
        commanded_rudder += channel_roll->get_servo_out() * g.kff_rudder_mix;
        commanded_rudder += rudder_input;
    }

    steering_control.rudder = constrain_int16(commanded_rudder, -4500, 4500);
}

/*
  calculate yaw control for ground steering with specific course
 */
void Plane::calc_nav_yaw_course(void)
{
    // holding a specific navigation course on the ground. Used in
    // auto-takeoff and landing
    int32_t bearing_error_cd = nav_controller->bearing_error_cd();
    steering_control.steering = steerController.get_steering_out_angle_error(bearing_error_cd);
    if (stick_mixing_enabled()) {
        stick_mix_channel(channel_rudder, steering_control.steering);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}

/*
  calculate yaw control for ground steering
 */
void Plane::calc_nav_yaw_ground(void)
{
    if (gps.ground_speed() < 1 && 
        channel_throttle->get_control_in() == 0 &&
        flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
        flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        // manual rudder control while still
        steer_state.locked_course = false;
        steer_state.locked_course_err = 0;
        steering_control.steering = rudder_input;
        return;
    }

    float steer_rate = (rudder_input/4500.0f) * g.ground_steer_dps;
    if (flight_stage == AP_SpdHgtControl::FLIGHT_TAKEOFF ||
        flight_stage == AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        steer_rate = 0;
    }
    if (!is_zero(steer_rate)) {
        // pilot is giving rudder input
        steer_state.locked_course = false;        
    } else if (!steer_state.locked_course) {
        // pilot has released the rudder stick or we are still - lock the course
        steer_state.locked_course = true;
        if (flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
            flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
            steer_state.locked_course_err = 0;
        }
    }
    if (!steer_state.locked_course) {
        // use a rate controller at the pilot specified rate
        steering_control.steering = steerController.get_steering_out_rate(steer_rate);
    } else {
        // use a error controller on the summed error
        int32_t yaw_error_cd = -ToDeg(steer_state.locked_course_err)*100;
        steering_control.steering = steerController.get_steering_out_angle_error(yaw_error_cd);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}


/*
  calculate a new nav_pitch_cd from the speed height controller
 */
void Plane::calc_nav_pitch()
{
    // Calculate the Pitch of the plane
    // --------------------------------
    int32_t commanded_pitch = SpdHgt_Controller->get_pitch_demand();

    // Received an external msg that guides roll in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_rpy_ms.y > 0 &&
            millis() - plane.guided_state.last_forced_rpy_ms.y < 3000) {
        commanded_pitch = plane.guided_state.forced_rpy_cd.y;
    }

    nav_pitch_cd = constrain_int32(commanded_pitch, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}


/*
  calculate a new nav_roll_cd from the navigation controller
 */
void Plane::calc_nav_roll()
{
    int32_t commanded_roll = nav_controller->nav_roll_cd();

    // Received an external msg that guides roll in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_rpy_ms.x > 0 &&
            millis() - plane.guided_state.last_forced_rpy_ms.x < 3000) {
        commanded_roll = plane.guided_state.forced_rpy_cd.x;
    }

    nav_roll_cd = constrain_int32(commanded_roll, -roll_limit_cd, roll_limit_cd);
    update_load_factor();
}


bool Plane::allow_reverse_thrust(void)
{
    // check if we should allow reverse thrust
    bool allow = false;

    if (g.use_reverse_thrust == USE_REVERSE_THRUST_NEVER) {
        return false;
    }

    switch (control_mode) {
    case AUTO:
        {
        uint16_t nav_cmd = mission.get_current_nav_cmd().id;

        // never allow reverse thrust during takeoff
        if (nav_cmd == MAV_CMD_NAV_TAKEOFF) {
            return false;
        }

        // always allow regardless of mission item
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_ALWAYS);

        // landing
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LAND_APPROACH) &&
                (nav_cmd == MAV_CMD_NAV_LAND);

        // LOITER_TO_ALT
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_TO_ALT) &&
                (nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT);

        // any Loiter (including LOITER_TO_ALT)
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_ALL) &&
                    (nav_cmd == MAV_CMD_NAV_LOITER_TIME ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TURNS ||
                     nav_cmd == MAV_CMD_NAV_LOITER_UNLIM);

        // waypoints
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_WAYPOINT) &&
                    (nav_cmd == MAV_CMD_NAV_WAYPOINT ||
                     nav_cmd == MAV_CMD_NAV_SPLINE_WAYPOINT);
        }
        break;

    case LOITER:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_LOITER);
        break;
    case RTL:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_RTL);
        break;
    case CIRCLE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CIRCLE);
        break;
    case CRUISE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CRUISE);
        break;
    case FLY_BY_WIRE_B:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_FBWB);
        break;
    case AVOID_ADSB:
    case GUIDED:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_GUIDED);
        break;
    default:
        // all other control_modes are auto_throttle_mode=false.
        // If we are not controlling throttle, don't limit it.
        allow = true;
        break;
    }

    return allow;
}

/*
  adjust nav_pitch_cd for STAB_PITCH_DOWN_CD. This is used to make
  keeping up good airspeed in FBWA mode easier, as the plane will
  automatically pitch down a little when at low throttle. It makes
  FBWA landings without stalling much easier.
 */
void Plane::adjust_nav_pitch_throttle(void)
{
    int8_t throttle = throttle_percentage();
    if (throttle >= 0 && throttle < aparm.throttle_cruise && flight_stage != AP_SpdHgtControl::FLIGHT_VTOL) {
        float p = (aparm.throttle_cruise - throttle) / (float)aparm.throttle_cruise;
        nav_pitch_cd -= g.stab_pitch_down * 100.0f * p;
    }
}


/*
  calculate a new aerodynamic_load_factor and limit nav_roll_cd to
  ensure that the load factor does not take us below the sustainable
  airspeed
 */
void Plane::update_load_factor(void)
{
    float demanded_roll = fabsf(nav_roll_cd*0.01f);
    if (demanded_roll > 85) {
        // limit to 85 degrees to prevent numerical errors
        demanded_roll = 85;
    }
    aerodynamic_load_factor = 1.0f / safe_sqrt(cosf(radians(demanded_roll)));

    if (!aparm.stall_prevention) {
        // stall prevention is disabled
        return;
    }
    if (fly_inverted()) {
        // no roll limits when inverted
        return;
    }

    float max_load_factor = smoothed_airspeed / aparm.airspeed_min;
    if (max_load_factor <= 1) {
        // our airspeed is below the minimum airspeed. Limit roll to
        // 25 degrees
        nav_roll_cd = constrain_int32(nav_roll_cd, -2500, 2500);
        roll_limit_cd = constrain_int32(roll_limit_cd, -2500, 2500);
    } else if (max_load_factor < aerodynamic_load_factor) {
        // the demanded nav_roll would take us past the aerodymamic
        // load limit. Limit our roll to a bank angle that will keep
        // the load within what the airframe can handle. We always
        // allow at least 25 degrees of roll however, to ensure the
        // aircraft can be maneuvered with a bad airspeed estimate. At
        // 25 degrees the load factor is 1.1 (10%)
        int32_t roll_limit = degrees(acosf(sq(1.0f / max_load_factor)))*100;
        if (roll_limit < 2500) {
            roll_limit = 2500;
        }
        nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit, roll_limit);
        roll_limit_cd = constrain_int32(roll_limit_cd, -roll_limit, roll_limit);
    }    
}


int16_t Plane::control_chirp(void)
{
  float dt;
  uint64_t now = AP_HAL::micros64();

   if (chirp.last_run_us == 0 || now - chirp.last_run_us > 200000UL) {
        // reset after not running for 0.2s
    chirp.t = 0;
    chirp.last_run_us = now;
    return 0;
    }
  
  dt = (now - chirp.last_run_us) * 1.0e-6f;

  float f0 = 0.1;
  float f1 = 0.5;
  float pi = 3.14159;
  float k = (f1-f0)/10; //chirp for 10 seconds

  float out = sinf(2*pi*(f0*chirp.t+(k/2)*chirp.t*chirp.t));
  out = 1500 + (500*out);

  chirp.t += dt;
  return ((int16_t)((float)(out+0.5)));
}
