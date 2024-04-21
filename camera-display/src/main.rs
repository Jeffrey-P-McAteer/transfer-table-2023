
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
    primitives::{PrimitiveStyle, PrimitiveStyleBuilder, Line, Rectangle},
    pixelcolor::{Rgb888, Bgr888},
    prelude::*,
    text::Text,
};


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

  let txt_bg_style = PrimitiveStyleBuilder::new()
    .stroke_color(Bgr888::BLACK)
    .stroke_width(0)
    .fill_color(Bgr888::BLACK)
    .build();

  let mut last_n_frame_times: [std::time::SystemTime; 8] = [std::time::SystemTime::now(); 8];
  // vv re-calculated off last_n_frame_times at regular intervals
  let mut rolling_fps_val: f32 = 0.0;

  let mut loop_i = 0;
  loop {
      loop_i += 1;
      if loop_i > 1000 {
        loop_i = 0;
      }

      let (frame_mjpg_buf, meta) = stream.next()?;

      last_n_frame_times[loop_i % last_n_frame_times.len()] = std::time::SystemTime::now();

      if loop_i % 2 == 0 {
        let mut frames_total_ms: f32 = 0.0;
        for i in 0..(last_n_frame_times.len()-1) {
          if let Ok(frame_t_dist) = last_n_frame_times[i+1].duration_since(last_n_frame_times[i]) {
            frames_total_ms += frame_t_dist.as_millis() as f32;
          }
        }
        rolling_fps_val = last_n_frame_times.len() as f32 / frames_total_ms; // frames-per-millisecond
        rolling_fps_val *= 1000.0; // frames-per-second
        println!("rolling_fps_val = {:?}", rolling_fps_val);
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
      }

      // Process the BGR/RGB/whatevs pixels, drawing onto &mut embed_fb

      for y in 0..cam_fmt_h {
        for x in 0..cam_fmt_w {
          let fb_px_offset = ( ((y*cam_fmt_w) + x) * fb_bpp) as usize;

          let r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          let jpeg_px_offset = (((y*cam_fmt_w) + x) * 3) as usize;

          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: y as i32 },
            embedded_graphics::pixelcolor::Bgr888::new(
              cam_pixels[jpeg_px_offset+2], cam_pixels[jpeg_px_offset+1], cam_pixels[jpeg_px_offset+0] // wierd - these look perfect, but imply the MJPG format is using BGR24!
            )
          );
        }
      }

      // Use luminance value to locate rails
      const table_rail_y: usize = 330;
      const layout_rail_y: usize = 350;
      const rail_pair_width_px: usize = 96; // measured center-to-center

      // Draw table_rail_y debug line
      Line::new(Point::new(0, table_rail_y as i32), Point::new(cam_fmt_w as i32, table_rail_y as i32))
        .into_styled(PrimitiveStyle::with_stroke(Bgr888::RED, 1))
        .draw(&mut embed_fb)?;

      Line::new(Point::new(0, layout_rail_y as i32), Point::new(cam_fmt_w as i32, layout_rail_y as i32))
        .into_styled(PrimitiveStyle::with_stroke(Bgr888::BLUE, 1))
        .draw(&mut embed_fb)?;

      // Black rectangle over remaining rightmost screen area

      Rectangle::new(
          Point::new(cam_fmt_w as i32,                      0),
          Size::new(EMBED_FB_W as u32 - cam_fmt_h as u32,   EMBED_FB_H as u32)
        )
        .into_styled(txt_bg_style)
        .draw(&mut embed_fb)?;

      let fps_txt = format!("FPS: {:.2}", rolling_fps_val);
      Text::new(&fps_txt, Point::new(EMBED_FB_W as i32 - 140, EMBED_FB_H as i32 - 60), font_style).draw(&mut embed_fb)?;


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

          let embed_fb_r_idx = embed_fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let embed_fb_g_idx = embed_fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let embed_fb_b_idx = embed_fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          if y < fb_h && y < EMBED_FB_H && x < fb_w && x < EMBED_FB_W {
            // Handle all observed bit cases the same way - we use a 1/2/3/4-byte unsigned integer to collect
            // bits, then mask them onto the 1/2/3/4 bytes in fb_mem.
            if fb_bpp == 2 {
              let mut pixels: u16 = 0;

              // Because of <8 bits, we must construct custom masks for each operation as 0xff is too wide!
              // .length specifies number of bits
              let r_mask = u16::MAX >> (16 - fb_pxlyt.red.length);
              let r_max_val: u16 = 2u16.pow(fb_pxlyt.red.length);

              let g_mask = u16::MAX >> (16 - fb_pxlyt.green.length);
              let g_max_val: u16 = 2u16.pow(fb_pxlyt.green.length);

              let b_mask = u16::MAX >> (16 - fb_pxlyt.blue.length);
              let b_max_val: u16 = 2u16.pow(fb_pxlyt.blue.length);

              if fb_pxlyt.red.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_r_idx] as usize] ) as u16) << fb_pxlyt.red.offset;
              }
              else if fb_pxlyt.red.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_r_idx] as usize] ) as u16) << fb_pxlyt.red.offset;
              }

              if fb_pxlyt.green.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_g_idx] as usize] ) as u16) << fb_pxlyt.green.offset;
              }
              else if fb_pxlyt.green.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_g_idx] as usize] ) as u16) << fb_pxlyt.green.offset;
              }

              if fb_pxlyt.blue.length == 5 {
                pixels |= (( EIGHT_TO_FIVE_BIT_TABLE[ embed_fb_data[embed_fb_b_idx] as usize] ) as u16) << fb_pxlyt.blue.offset;
              }
              else if fb_pxlyt.blue.length == 6 {
                pixels |= (( EIGHT_TO_SIX_BIT_TABLE[ embed_fb_data[embed_fb_b_idx] as usize] ) as u16) << fb_pxlyt.blue.offset;
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

          /*
          let fb_r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let fb_g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let fb_b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          if y < fb_h && y < EMBED_FB_H && x < fb_w && x < EMBED_FB_W {
            fb_mem[fb_r_idx] = embed_fb_data[embed_fb_r_idx];
            fb_mem[fb_g_idx] = embed_fb_data[embed_fb_g_idx];
            fb_mem[fb_b_idx] = embed_fb_data[embed_fb_b_idx];
          }
          else {
            fb_mem[fb_r_idx] = 0x00;
            fb_mem[fb_g_idx] = 0x00;
            fb_mem[fb_b_idx] = 0x00;
          }
          */
        }
      }


      // Give system 2ms of delay after each frame
      std::thread::sleep(std::time::Duration::from_millis(2));

  }

  Ok(())
}









