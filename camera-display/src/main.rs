
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
    primitives::{PrimitiveStyle, Line},
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

  // See https://docs.rs/embedded-graphics/latest/embedded_graphics/mono_font/ascii/index.html
  let font_style = MonoTextStyle::new(&mono_font::ascii::FONT_9X18_BOLD, Bgr888::WHITE);

  // 800x480 is the design size of the Pi's monitor
  const EMBED_FB_H: usize = 480;
  const EMBED_FB_W: usize = 800;
  const EMBED_FB_BPP: usize = 3; // Assumed
  let mut embed_fb = Framebuffer::<Bgr888, _, LittleEndian, EMBED_FB_W, EMBED_FB_H, { buffer_size::<Bgr888>(EMBED_FB_W, EMBED_FB_H) }>::new();


  let mut loop_i = 0;
  loop {
      loop_i += 1;
      if loop_i > 1000 {
        loop_i = 0;
      }

      let (frame_mjpg_buf, meta) = stream.next()?;

      if loop_i % 20 == 0 {
        println!(
            "Buffer size: {}, seq: {}, timestamp: {}",
            frame_mjpg_buf.len(),
            meta.sequence,
            meta.timestamp
        );
      }

      let mut jpeg_decoder = jpeg_decoder::Decoder::new(BufReader::new(frame_mjpg_buf));

      let cam_pixels = jpeg_decoder.decode()?;
      const cam_bpp: usize = 3; // Cam pixels is always in RGB24 format! Yay! \o/

      if loop_i % 20 == 0 { // We pretty much always see RGB24
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




      Text::new("Text Render\nTest", Point::new(EMBED_FB_W as i32 - 140, EMBED_FB_H as i32 - 60), font_style).draw(&mut embed_fb)?;


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
          let embed_fb_px_offset = if cfg!(target_arch="aarch64") || cfg!(target_arch="arm") {
            (( ((fb_h-1)-y) *EMBED_FB_W) + ((fb_w-1)-x) ) * EMBED_FB_BPP
          }
          else {
            ((y*EMBED_FB_W) + x) * EMBED_FB_BPP
          } as usize;

          let embed_fb_r_idx = embed_fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let embed_fb_g_idx = embed_fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let embed_fb_b_idx = embed_fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          if y < fb_h && y < EMBED_FB_H && x < fb_w && x < EMBED_FB_W {
            // Handle all observed bit cases the same way - we use a 1/2/3/4-byte unsigned integer to collect
            // bits, then mask them onto the 1/2/3/4 bytes in fb_mem.
            if fb_bpp == 2 {
              let mut pixels: u16 = 0;


              fb_mem[fb_px_offset + 0] = 0x00;
              fb_mem[fb_px_offset + 1] = 0x00;
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


  }

  Ok(())
}









