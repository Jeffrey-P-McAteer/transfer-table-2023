
use std::io::BufReader;
use jpeg_decoder::Decoder;

use v4l::buffer::Type;
use v4l::io::mmap::Stream;
use v4l::io::traits::CaptureStream;
use v4l::video::Capture;
use v4l::Device;
use v4l::FourCC;

use embedded_graphics::{
    framebuffer,
    framebuffer::{Framebuffer, buffer_size},
    pixelcolor::{raw::LittleEndian},
    mono_font,
    mono_font::{MonoTextStyle},
    primitives::{PrimitiveStyle, PrimitiveStyleBuilder, Line, Rectangle, Triangle},
    pixelcolor::{Rgb888, Bgr888},
    prelude::*,
    text::Text,
};

use rand::prelude::*;


const GPIO_MOTOR_KEYS_IN_DIR: &'static str = "/tmp/gpio_motor_keys_in";


fn main() {
  // Attempt to chvt 7
  let unused = std::process::Command::new("chvt")
    .args(&["7"])
    .status();
  let unused = std::process::Command::new("sysctl") // From https://bbs.archlinux.org/viewtopic.php?id=284267
    .args(&["kernel.printk=0 4 0 4"])
    .status();

  loop {
    if let Err(e) = do_camera_loop() {
      println!("[ do_camera_loop exited ] {:?}", e);
    }
    std::thread::sleep(std::time::Duration::from_millis(1200));
  }
}

const EIGHT_TO_FIVE_BIT_TABLE: [u8; 256] = [
  0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,2,
  2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,4,4,
  4,4,4,4,4,4,5,5,5,5,5,5,5,5,6,6,6,
  6,6,6,6,6,7,7,7,7,7,7,7,7,8,8,8,8,
  8,8,8,8,9,9,9,9,9,9,9,9,10,10,10,10,10,
  10,10,10,11,11,11,11,11,11,11,11,12,12,12,12,12,12,
  12,12,13,13,13,13,13,13,13,13,14,14,14,14,14,14,14,
  14,15,15,15,15,15,15,15,15,16,16,16,16,16,16,16,16,
  17,17,17,17,17,17,17,17,18,18,18,18,18,18,18,18,19,
  19,19,19,19,19,19,19,20,20,20,20,20,20,20,20,21,21,
  21,21,21,21,21,21,22,22,22,22,22,22,22,22,23,23,23,
  23,23,23,23,23,24,24,24,24,24,24,24,24,25,25,25,25,
  25,25,25,25,26,26,26,26,26,26,26,26,27,27,27,27,27,
  27,27,27,28,28,28,28,28,28,28,28,29,29,29,29,29,29,
  29,29,30,30,30,30,30,30,30,30,31,31,31,31,31,31,31,
  31,
];


const EIGHT_TO_SIX_BIT_TABLE: [u8; 256] = [
  0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,
  4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,
  8,8,9,9,9,9,10,10,10,10,11,11,11,11,12,12,12,
  12,13,13,13,13,14,14,14,14,15,15,15,15,16,16,16,16,
  17,17,17,17,18,18,18,18,19,19,19,19,20,20,20,20,21,
  21,21,21,22,22,22,22,23,23,23,23,24,24,24,24,25,25,
  25,25,26,26,26,26,27,27,27,27,28,28,28,28,29,29,29,
  29,30,30,30,30,31,31,31,31,32,32,32,32,33,33,33,33,
  34,34,34,34,35,35,35,35,36,36,36,36,37,37,37,37,38,
  38,38,38,39,39,39,39,40,40,40,40,41,41,41,41,42,42,
  42,42,43,43,43,43,44,44,44,44,45,45,45,45,46,46,46,
  46,47,47,47,47,48,48,48,48,49,49,49,49,50,50,50,50,
  51,51,51,51,52,52,52,52,53,53,53,53,54,54,54,54,55,
  55,55,55,56,56,56,56,57,57,57,57,58,58,58,58,59,59,
  59,59,60,60,60,60,61,61,61,61,62,62,62,62,63,63,63,
  63,
];


// Ought to run infinitely, returns result so we can handle hardware disconnect & re-connects
#[allow(unreachable_code)]
fn do_camera_loop() -> Result<(), Box<dyn std::error::Error>> {
  let mut dev = Device::new(0)?;

  // Let's say we want to explicitly request another format
  let mut fmt = dev.format()?;
  fmt.width = 640;
  fmt.height = 480;
  fmt.fourcc = FourCC::new(b"MJPG");

  let fmt = dev.set_format(&fmt)?;

  // The actual format chosen by the device driver may differ from what we
  // requested! Print it out to get an idea of what is actually used now.
  println!("Camera Image Format in use:\n{}", fmt);

  let cam_fmt_h = fmt.height as usize;
  let cam_fmt_w = fmt.width as usize;
  let img_bpp = (fmt.size / (fmt.height * fmt.width)) as usize;
  println!("Camera img_bpp = {:?}", img_bpp);

  // Now we'd like to capture some frames!
  // First, we need to create a stream to read buffers from. We choose a
  // mapped buffer stream, which uses mmap to directly access the device
  // frame buffer. No buffers are copied nor allocated, so this is actually
  // a zero-copy operation.

  // To achieve the best possible performance, you may want to use a
  // UserBufferStream instance, but this is not supported on all devices,
  // so we stick to the mapped case for this example.
  // Please refer to the rustdoc docs for a more detailed explanation about
  // buffer transfers.

  // Create the stream, which will internally 'allocate' (as in map) the
  // number of requested buffers for us.
  let mut stream = Stream::with_buffers(&mut dev, Type::VideoCapture, 1)?;

  // At this point, the stream is ready and all buffers are setup.
  // We can now read frames (represented as buffers) by iterating through
  // the stream. Once an error condition occurs, the iterator will return
  // None.

  let devices = linuxfb::Framebuffer::list()?;
  if devices.len() < 1 {
    println!("No Framebufffer devices found, exiting!");
    return Ok(());
  }

  let fb_device = devices[0].clone();
  println!("Using fb_device = {:?}", fb_device);

  // Open framebuffer for output
  let fb = match linuxfb::Framebuffer::new(fb_device) {
    Ok(f) => f,
    Err(e) => {
        println!("linuxfb::Framebuffer::new() e = {:?}", e);
        return Ok(());
    }
  };

  let (fb_w, fb_h) = fb.get_size();
  let fb_w = fb_w as usize;
  let fb_h = fb_h as usize;
  println!("FB Size in pixels: {:?}", fb.get_size());
  let fb_bpp = fb.get_bytes_per_pixel() as usize;
  println!("FB Bytes per pixel: {:?}", fb_bpp);
  let fb_pxlyt = fb.get_pixel_layout();
  println!("FB Pixel layout: {:?}", fb_pxlyt);

  // Ensure screen is on
  if let Err(e) = fb.blank(linuxfb::BlankingLevel::Unblank) {
    println!("fb.blank() e = {:?}", e);
  }

  // 800x480 is the design size of the Pi's monitor
  const EMBED_FB_H: usize = 480;
  const EMBED_FB_W: usize = 800;
  const EMBED_FB_BPP: usize = 3; // Assumed
  let mut embed_fb = Framebuffer::<Bgr888, _, LittleEndian, EMBED_FB_W, EMBED_FB_H, { buffer_size::<Bgr888>(EMBED_FB_W, EMBED_FB_H) }>::new();

  // See https://docs.rs/embedded-graphics/latest/embedded_graphics/mono_font/ascii/index.html
  let font_style = MonoTextStyle::new(&mono_font::ascii::FONT_9X18_BOLD, Bgr888::WHITE);
  let green_font_style = MonoTextStyle::new(&mono_font::ascii::FONT_9X18_BOLD, Bgr888::GREEN);
  let red_font_style = MonoTextStyle::new(&mono_font::ascii::FONT_9X18_BOLD, Bgr888::RED);
  let yellow_font_style = MonoTextStyle::new(&mono_font::ascii::FONT_9X18_BOLD, Bgr888::YELLOW);

  let txt_bg_style = PrimitiveStyleBuilder::new()
    .stroke_color(Bgr888::BLACK)
    .stroke_width(0)
    .fill_color(Bgr888::BLACK)
    .build();

  let dbg_arrow_solid_green_style = PrimitiveStyleBuilder::new()
    .stroke_color(Bgr888::CSS_DARK_GREEN)
    .stroke_width(1)
    .fill_color(Bgr888::GREEN)
    .build();

  let mut last_n_frame_times: [std::time::SystemTime; 8] = [std::time::SystemTime::now(); 8];
  // vv re-calculated off last_n_frame_times at regular intervals
  let mut rolling_fps_val: f32 = 0.0;

  // Used to allow the layout rail, which is ASSUMED STATIONARY, to not need
  // to always be detected. If we do not detect it, the average of values >=0 in
  // this array will be used for targeting the table's corrected position.
  let mut last_n_layout_rail_x_positions: [i32; 8] = [-1; 8];

  const NUM_MOVES_ALLOWED_FOR_CORRECTION: isize = 32;
  let mut num_remaining_correction_moves: isize = NUM_MOVES_ALLOWED_FOR_CORRECTION;

  let mut have_saved_this_correction_pos: bool = false;

  let mut loop_i = 0;
  loop {
      loop_i += 1;
      if loop_i > 100000000 {
        loop_i = 0;
      }

      let (frame_mjpg_buf, meta) = stream.next()?;

      last_n_frame_times[loop_i % last_n_frame_times.len()] = std::time::SystemTime::now();

      {
        let mut frames_total_ms: f32 = 0.0;
        for i in 0..(last_n_frame_times.len()-1) {
          if let Ok(frame_t_dist) = last_n_frame_times[i+1].duration_since(last_n_frame_times[i]) {
            frames_total_ms += frame_t_dist.as_millis() as f32;
          }
        }
        rolling_fps_val = last_n_frame_times.len() as f32 / frames_total_ms; // frames-per-millisecond
        rolling_fps_val *= 1000.0; // frames-per-second

        if loop_i % 25 == 0 {
          println!("rolling_fps_val = {:?}", rolling_fps_val);
        }
      }

      if loop_i % 25 == 0 {
        println!(
            "Buffer size: {}, seq: {}, timestamp: {}",
            frame_mjpg_buf.len(),
            meta.sequence,
            meta.timestamp
        );
      }

      if loop_i % 100 == 0 {
        // Ensure screen is on
        if let Err(e) = fb.blank(linuxfb::BlankingLevel::Unblank) {
          println!("fb.blank() e = {:?}", e);
        }
      }


      let mut jpeg_decoder = jpeg_decoder::Decoder::new(BufReader::new(frame_mjpg_buf));

      let cam_pixels = jpeg_decoder.decode()?;
      const cam_bpp: usize = 3; // Cam pixels is always in RGB24 format! Yay! \o/

      if loop_i % 25 == 0 { // We pretty much always see RGB24
        if let Some(jpg_info) = jpeg_decoder.info() {
          println!("jpg_info = {:?}", jpg_info);
        }
        println!("fb_pxlyt = {:?}", fb_pxlyt);
      }

      // Process the BGR/RGB/whatevs pixels, drawing onto &mut embed_fb

      for y in 0..cam_fmt_h {
        for x in 0..cam_fmt_w {

          let jpeg_px_offset = (((y*cam_fmt_w) + x) * 3) as usize;

          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: y as i32 },
            embedded_graphics::pixelcolor::Bgr888::new(
              //cam_pixels[jpeg_px_offset+2], cam_pixels[jpeg_px_offset+1], cam_pixels[jpeg_px_offset+0] // wierd - these look perfect, but imply the MJPG format is using BGR24!
              cam_pixels[jpeg_px_offset+0], cam_pixels[jpeg_px_offset+1], cam_pixels[jpeg_px_offset+2]
            )
          );
        }
      }

      //const table_rail_y: usize = 330; // Measures OK by photos from webserver.py, but wrong by hardware measurement.
      //const layout_rail_y: usize = 350;

      const table_rail_y: usize = 346;
      const layout_rail_y: usize = 368;

      //const rail_pair_width_px: usize = 96; // measured center-to-center
      const rail_pair_width_px: usize = 106; // measured using debug txt


      const rail_max_err: usize = 1; // Allow one rail center to be eg x1=50 and x2=51 without moving table, but x=52 will cause movement!

      let mut rail_dbg_txt = "".to_string();

      // Draw table_rail_y debug line
      Line::new(Point::new(0, table_rail_y as i32), Point::new(cam_fmt_w as i32, table_rail_y as i32))
        .into_styled(PrimitiveStyle::with_stroke(Bgr888::CSS_DARK_MAGENTA, 1))
        .draw(&mut embed_fb)?;

      Line::new(Point::new(0, layout_rail_y as i32), Point::new(cam_fmt_w as i32, layout_rail_y as i32))
        .into_styled(PrimitiveStyle::with_stroke(Bgr888::CSS_HOT_PINK, 1))
        .draw(&mut embed_fb)?;


      let mut table_rail_brightness: Vec<u8> = vec![0; cam_fmt_w];
      let mut max_table_rail_brightness: u8 = 0;
      let mut layout_rail_brightnesses: Vec<u8> = vec![0; cam_fmt_w];
      let mut max_layout_rail_brightness: u8 = 0;

      for x in 0..cam_fmt_w {
        let jpeg_px_offset = (((table_rail_y*cam_fmt_w) + x) * 3) as usize;
        let b = brightness_from_px(
          cam_pixels[jpeg_px_offset+0] as f32, // R
          cam_pixels[jpeg_px_offset+1] as f32, // G
          cam_pixels[jpeg_px_offset+2] as f32  // B
        );
        table_rail_brightness[x] = b;
        if b > max_table_rail_brightness {
          max_table_rail_brightness = b;
        }
      }

      for x in 0..cam_fmt_w {
        let jpeg_px_offset = (((layout_rail_y*cam_fmt_w) + x) * 3) as usize;
        let b = brightness_from_px(
          cam_pixels[jpeg_px_offset+0] as f32, // R
          cam_pixels[jpeg_px_offset+1] as f32, // G
          cam_pixels[jpeg_px_offset+2] as f32  // B
        );
        layout_rail_brightnesses[x] = b;
        if b > max_layout_rail_brightness {
          max_layout_rail_brightness = b;
        }
      }

      // Now we do a boolean on the brightness measures, selecting the top 15% of pixels as "potential rails"
      let lowest_table_rail_brightness = ((max_table_rail_brightness as f32) - ((max_table_rail_brightness as f32) * 0.14)) as u8;
      let lowest_layout_rail_brightness = ((max_layout_rail_brightness as f32) - ((max_layout_rail_brightness as f32) * 0.14)) as u8;

      let mut table_maybe_rails: Vec<bool> = vec![false; cam_fmt_w];
      let mut layout_maybe_rails: Vec<bool> = vec![false; cam_fmt_w];

      for x in 0..cam_fmt_w {
        if table_rail_brightness[x] >= lowest_table_rail_brightness {
          table_maybe_rails[x] = true;
        }
        if layout_rail_brightnesses[x] >= lowest_layout_rail_brightness {
          layout_maybe_rails[x] = true;
        }
      }

      { // Add a debug line for the maybe_rails results
        for x in 0..cam_fmt_w {
          let table_color =  if table_maybe_rails[x] {
            embedded_graphics::pixelcolor::Bgr888::WHITE
          }
          else {
            embedded_graphics::pixelcolor::Bgr888::BLACK
          };
          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: (table_rail_y+1) as i32 }, table_color
          );
          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: (table_rail_y+2) as i32 }, table_color
          );

          let layout_color =  if layout_maybe_rails[x] {
            embedded_graphics::pixelcolor::Bgr888::WHITE
          }
          else {
            embedded_graphics::pixelcolor::Bgr888::BLACK
          };
          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: (layout_rail_y+1) as i32 }, layout_color
          );
          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: (layout_rail_y+2) as i32 }, layout_color
          );


        }
      }

      // Now we pick the first offset where layout_maybe_rails[x + rail_pair_width_px] is ALSO a maybe rail.
      const skipped_edge_pixels: usize = 80;
      let mut table_rail_x: Option<u32> = None;
      let mut layout_rail_x: Option<u32> = None;
      for x in skipped_edge_pixels..((cam_fmt_w-rail_pair_width_px)-skipped_edge_pixels) {

        // Write debug message for _all_ where we go from !table_maybe_rails[x] -> table_maybe_rails[x] && [x+1] (all black -> 2x white px)
        /*if x >= 1 && x <(cam_fmt_w-1) && !table_maybe_rails[x-1] && table_maybe_rails[x] && table_maybe_rails[x+1] {
          rail_dbg_txt = format!("{}\n{}", &rail_dbg_txt.clone(), x );
        }*/

        if table_rail_x.is_none() && table_maybe_rails[x] && table_maybe_rails[x+rail_pair_width_px] {
          // Found it! Seek forwards until !table_maybe_rails[x+n] and record the CENTER of left-most rail.
          let mut x_end = x;
          for n in x..cam_fmt_w {
            if !table_maybe_rails[n] {
              x_end = n;
              break;
            }
          }
          table_rail_x = Some( ((x + x_end) / 2) as u32 );
        }
        if layout_rail_x.is_none() && layout_maybe_rails[x] && layout_maybe_rails[x+rail_pair_width_px] {
          // Found it! Seek forwards until !layout_maybe_rails[x+n] and record the CENTER of left-most rail.
          let mut x_end = x;
          for n in x..cam_fmt_w {
            if !layout_maybe_rails[n] {
              x_end = n;
              break;
            }
          }
          layout_rail_x = Some( ((x + x_end) / 2) as u32 );
        }
      }

      if let Some(measured_layout_rail_x) = layout_rail_x {
        // Record for book-keeping
        last_n_layout_rail_x_positions[loop_i % last_n_layout_rail_x_positions.len()] = measured_layout_rail_x as i32;
      }
      else {
        // Allow for prior layout_rail_x values to be assigned in IF we do not have a real measurement to use.
        let mut avg_last_n_layout_rail_x_positions: i32 = -1;
        let mut num_last_n_layout_rail_x_positions: i32 = 0;
        for i in 0..last_n_layout_rail_x_positions.len() {
          if last_n_layout_rail_x_positions[i] > 0 {
            avg_last_n_layout_rail_x_positions += last_n_layout_rail_x_positions[i];
          }
        }
        if num_last_n_layout_rail_x_positions > 0 {
          avg_last_n_layout_rail_x_positions /= num_last_n_layout_rail_x_positions;
          layout_rail_x = Some( avg_last_n_layout_rail_x_positions as u32 );
        }
      }

      // Did we find rails?
      let mut rail_msg = "[ NO RAIL ]".to_string();
      let mut rail_msg_style = red_font_style;

      let mut table_control_code_to_write: Option<usize> = None;

      if let Some(table_rail_x) = table_rail_x {
        // Write debug BLACK pixels
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: table_rail_x as i32, y: (table_rail_y+2) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: (table_rail_x+1) as i32, y: (table_rail_y+2) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: table_rail_x as i32, y: (table_rail_y+3) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: (table_rail_x+1) as i32, y: (table_rail_y+3) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
      }
      if let Some(layout_rail_x) = layout_rail_x {
        // Write debug BLACK pixels
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: layout_rail_x as i32, y: (layout_rail_y+2) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: (layout_rail_x+1) as i32, y: (layout_rail_y+2) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: layout_rail_x as i32, y: (layout_rail_y+3) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
        embed_fb.set_pixel(
          embedded_graphics::geometry::Point { x: (layout_rail_x+1) as i32, y: (layout_rail_y+3) as i32 }, embedded_graphics::pixelcolor::Bgr888::RED
        );
      }

      if let (Some(table_rail_x), Some(layout_rail_x)) = (table_rail_x, layout_rail_x)  {
        // Compute state info
        let rail_diff: i32 = table_rail_x as i32 - layout_rail_x as i32;
        if rail_diff.abs() <= rail_max_err as i32 {
          rail_msg = "RAILS ALIGNED".to_string();
          rail_msg_style = green_font_style;
        }
        else {
          if rail_diff < 0 {
            rail_msg = "MOVING LEFT".to_string();
            rail_msg_style = yellow_font_style;
            table_control_code_to_write = Some(114); // TODO may be baclwards; if so just swap directions / codes

            // Debug triangle
            Triangle::new(
                  Point::new(table_rail_x as i32,                       (table_rail_y - 48) as i32 ),
                  Point::new((table_rail_x as i32 + rail_diff.abs()) as i32,  (table_rail_y - 40) as i32 ),
                  Point::new(table_rail_x as i32,                       (table_rail_y - 32) as i32 )
              )
              .into_styled(dbg_arrow_solid_green_style)
              .draw(&mut embed_fb)?;

          }
          else {
            rail_msg = "MOVING RIGHT".to_string();
            rail_msg_style = yellow_font_style;
            table_control_code_to_write = Some(115);

            // Debug triangle
            Triangle::new(
                  Point::new(table_rail_x as i32,                       (table_rail_y - 48) as i32 ),
                  Point::new((table_rail_x as i32 - rail_diff.abs()) as i32,  (table_rail_y - 40) as i32 ),
                  Point::new(table_rail_x as i32,                       (table_rail_y - 32) as i32 )
              )
              .into_styled(dbg_arrow_solid_green_style)
              .draw(&mut embed_fb)?;


          }
        }
      }
      else {
        rail_msg = "[ NO RAIL ]".to_string();
        rail_msg_style = red_font_style;
      }

      // Read table info - are we moving? How long since table last moved?
      let mut motor_is_moving = true;
      let mut motor_has_not_moved_recently = true;
      let mut motor_state_msg = "NO MOTOR DATA".to_string();
      let mut motor_state_msg_style = red_font_style;
      let mut automove_active = false;

      if std::path::Path::new("/tmp/gpio_motor_is_active").exists() {
        motor_state_msg = "MOTOR MOVING\nAUTO-MOVE OFF".to_string();
        motor_state_msg_style = red_font_style;
        motor_is_moving = true;
        automove_active = false;
        num_remaining_correction_moves = NUM_MOVES_ALLOWED_FOR_CORRECTION;
        have_saved_this_correction_pos = true;
      }
      else {
        motor_is_moving = false;
        motor_state_msg = "MOTOR STOPPED\nAUTO-MOVE ON".to_string();
        motor_state_msg_style = yellow_font_style;
        automove_active = true;
        have_saved_this_correction_pos = false;
      }

      if let Ok(meta) = std::fs::metadata("/tmp/gpio_motor_last_active_mtime") {
        if let Ok(gpio_motor_last_active_mtime) = meta.modified() {
          let seconds_since_table_motion = std::time::SystemTime::now().duration_since(gpio_motor_last_active_mtime);
          if let Ok(seconds_since_table_motion) = seconds_since_table_motion {
            if seconds_since_table_motion.as_millis() > 9000 {
              motor_state_msg = "MOTOR STOPPED\nAUTO-MOVE OFF".to_string();
              motor_state_msg_style = green_font_style;
              automove_active = false;
              num_remaining_correction_moves = NUM_MOVES_ALLOWED_FOR_CORRECTION;
            }
          }
        }
      }

      // Make decisions if allowed to!
      if !motor_is_moving && automove_active && num_remaining_correction_moves >= 1 {
        if let Some(key_code) = table_control_code_to_write {
          num_remaining_correction_moves -= 1;
          let txt_file_num = rand::random::<u32>();
          let txt_file_path = format!("{}/{}.txt", GPIO_MOTOR_KEYS_IN_DIR, txt_file_num);
          let codes_str = format!("{}", key_code);
          rail_dbg_txt = codes_str.clone();
          if let Err(e) = std::fs::write(&txt_file_path, &codes_str) {
            println!("Error writing to {}: {:?}", &txt_file_path, e);
          }

        }
      }

      // If we finished moving, and did not run OUT of correction moved, write '=' keyvode to save table pos
      if !motor_is_moving && !automove_active && !have_saved_this_correction_pos && num_remaining_correction_moves > 1 {
        let knob_down_and_equals_codes = "113,14";
        let txt_file_num = rand::random::<u32>();
        let txt_file_path = format!("{}/{}.txt", GPIO_MOTOR_KEYS_IN_DIR, txt_file_num);
        if let Err(e) = std::fs::write(&txt_file_path, knob_down_and_equals_codes) {
          println!("Error writing to {}: {:?}", &txt_file_path, e);
        }
        rail_dbg_txt = knob_down_and_equals_codes.to_string();
        have_saved_this_correction_pos = true;
      }


      // Black rectangle over remaining rightmost screen area

      Rectangle::new(
          Point::new(cam_fmt_w as i32,                      0),
          Size::new(EMBED_FB_W as u32 - cam_fmt_h as u32,   EMBED_FB_H as u32)
        )
        .into_styled(txt_bg_style)
        .draw(&mut embed_fb)?;

      // rail_dbg_txt
      Text::new(&rail_dbg_txt, Point::new(EMBED_FB_W as i32 - 150, 10 ), red_font_style).draw(&mut embed_fb)?;

      Text::new(&motor_state_msg, Point::new(EMBED_FB_W as i32 - 150, EMBED_FB_H as i32 - 120), motor_state_msg_style).draw(&mut embed_fb)?;

      Text::new(&rail_msg, Point::new(EMBED_FB_W as i32 - 150, EMBED_FB_H as i32 - 60), rail_msg_style).draw(&mut embed_fb)?;

      let fps_txt = format!("FPS: {:.2}", rolling_fps_val);
      Text::new(&fps_txt, Point::new(EMBED_FB_W as i32 - 150, EMBED_FB_H as i32 - 20), font_style).draw(&mut embed_fb)?;


      // send to framebuffer!
      let mut fb_mem = match fb.map() {
        Ok(m) => m,
        Err(e) => {
          println!("fb.map() e = {:?}", e);
          return Ok(());
        }
      };

      let embed_fb_data = embed_fb.data();
      for y in 0..fb_h {
        for x in 0..fb_w {

          let fb_px_offset = ( ((y*fb_w) + x) * fb_bpp) as usize;

          // Conditionally flip what we're reading from if compiling on the pi
          /*let embed_fb_px_offset = if cfg!(target_arch="aarch64") || cfg!(target_arch="arm") {
            (( ((fb_h-1)-y) *EMBED_FB_W) + ((fb_w-1)-x) ) * EMBED_FB_BPP
          }
          else {
            ((y*EMBED_FB_W) + x) * EMBED_FB_BPP
          } as usize;*/

          let embed_fb_px_offset = (((y*EMBED_FB_W) + x) * EMBED_FB_BPP) as usize;

          let embed_fb_r_idx = embed_fb_px_offset + (0) as usize;
          let embed_fb_g_idx = embed_fb_px_offset + (1) as usize;
          let embed_fb_b_idx = embed_fb_px_offset + (2) as usize;

          if y < fb_h && y < EMBED_FB_H && x < fb_w && x < EMBED_FB_W {
            // Handle all observed bit cases the same way - we use a 1/2/3/4-byte unsigned integer to collect
            // bits, then mask them onto the 1/2/3/4 bytes in fb_mem.
            if fb_bpp == 2 {
              let mut pixels: u16 = 0;

              // fb_pxlyt = PixelLayout { // Observed off HW
              //   red: PixelLayoutChannel { offset: 11, length: 5, msb_right: false },
              //   green: PixelLayoutChannel { offset: 5, length: 6, msb_right: false },
              //   blue: PixelLayoutChannel { offset: 0, length: 5, msb_right: false },
              //   alpha: PixelLayoutChannel { offset: 0, length: 0, msb_right: false } }

              if fb_pxlyt.blue.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_b_idx] as usize] ) as u16) << fb_pxlyt.blue.offset;
              }
              else if fb_pxlyt.blue.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_b_idx] as usize] ) as u16) << fb_pxlyt.blue.offset;
              }

              if fb_pxlyt.green.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_g_idx] as usize] ) as u16) << fb_pxlyt.green.offset;
              }
              else if fb_pxlyt.green.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_g_idx] as usize] ) as u16) << fb_pxlyt.green.offset;
              }

              if fb_pxlyt.red.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_r_idx] as usize] ) as u16) << fb_pxlyt.red.offset;
              }
              else if fb_pxlyt.red.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_r_idx] as usize] ) as u16) << fb_pxlyt.red.offset;
              }

              //pixels |= ((embed_fb_data[embed_fb_g_idx] as u16) & g_mask) << fb_px[lyt.green.offset;
              //pixels |= ((embed_fb_data[embed_fb_b_idx] as u16) & b_mask) << fb_pxlyt.blue.offset;

              fb_mem[fb_px_offset + 0] = ((pixels >> 0) & 0xff) as u8;
              fb_mem[fb_px_offset + 1] = ((pixels >> 8) & 0xff) as u8;
            }
            else if fb_bpp == 3 || fb_bpp == 4 { // 4 just means "RGBA", which can be treated as rgb
              let mut pixels: u32 = 0;

              pixels |= (embed_fb_data[embed_fb_r_idx] as u32) << fb_pxlyt.red.offset;
              pixels |= (embed_fb_data[embed_fb_g_idx] as u32) << fb_pxlyt.green.offset;
              pixels |= (embed_fb_data[embed_fb_b_idx] as u32) << fb_pxlyt.blue.offset;

              fb_mem[fb_px_offset + 0] = ((pixels >> fb_pxlyt.blue.offset) & 0xff) as u8; // fb_mem[ + 0 ] is blue channel
              fb_mem[fb_px_offset + 1] = ((pixels >> fb_pxlyt.green.offset) & 0xff) as u8; // fb_mem[ +1 ] is green channel
              fb_mem[fb_px_offset + 2] = ((pixels >> fb_pxlyt.red.offset) & 0xff) as u8; //  fb_mem[ +2 ] is red channel

            }
          }
          else {
            // Just write 0s for all N bytes
            for i in 0..fb_bpp {
              fb_mem[fb_px_offset + i] = 0x00;
            }
          }

        }
      }


      // Give system 2ms of delay after each frame
      std::thread::sleep(std::time::Duration::from_millis(2));

  }

  Ok(())
}

fn brightness_from_px(r:f32, g:f32, b:f32) -> u8 {
  // Fast approx from https://stackoverflow.com/a/596241
  let weighted_sum: f32 = r+r+r+b+g+g+g+g;
  let mut val: f32 = weighted_sum / 6.0;
  if val > 255.0 {
    val = 255.0;
  }
  else if val < 0.0 {
    val = 0.0;
  }
  return val as u8;
}







