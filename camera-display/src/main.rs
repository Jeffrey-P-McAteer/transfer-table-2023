
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
    pixelcolor::{Rgb888, Bgr888},
    prelude::*,
    text::Text,
};




const GPIO_MOTOR_KEYS_IN_DIR: &'static str = "/tmp/gpio_motor_keys_in";


fn main() {
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
  fmt.fourcc = FourCC::new(b"YUYV"); // YCbCr 4:2:2 pixels
  // See https://fourcc.org/pixel-format/yuv-yuy2/

  // fmt.fourcc = FourCC::new(b"MJPG"); // Slow!

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

  // Allocate a buffer to use for processing image as RGB/BGR pixels
  // (will end up matching the framebuffer pixel order)
  let mut cam_rgb_buf: Vec<u8> = Vec::with_capacity(cam_fmt_h * cam_fmt_w * fb_bpp);
  cam_rgb_buf.resize(cam_fmt_h * cam_fmt_w * fb_bpp, 0); // zero buffer

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

      let (frame_yuv_buf, meta) = stream.next()?;

      if loop_i % 20 == 0 {
        println!(
            "Buffer size: {}, seq: {}, timestamp: {}",
            frame_yuv_buf.len(),
            meta.sequence,
            meta.timestamp
        );
      }

      let mut fb_mem = match fb.map() {
        Ok(m) => m,
        Err(e) => {
          println!("fb.map() e = {:?}", e);
          return Ok(());
        }
      };

      // Decode YCbCr 4:2:2 pixels into BGR pixels

      for i in 0..cam_rgb_buf.len() {
        cam_rgb_buf[i] = 0;
      }

      for y in 0..cam_fmt_h {
        for x in 0..cam_fmt_w {
          let fb_px_offset = ( ((y*cam_fmt_h) + x) * fb_bpp) as usize;
          /*if fb_px_offset+fb_bpp > cam_rgb_buf.len() {
            continue;
          }*/

          let r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          // TODO use this as pixel offset calculation reference - https://i.stack.imgur.com/Vprp4.png

          let camera_px_offset = (((y*cam_fmt_h) + x) * img_bpp) as usize;
          /*if camera_px_offset+img_bpp > frame_yuv_buf.len() {
            continue;
          }*/

          //let frame_y = frame_yuv_buf[((y*cam_fmt_h) + x) as usize]; // top 8 bits
          let frame_y = frame_yuv_buf[camera_px_offset as usize]; // top 8 bits
          let frame_u = 63; // frame_yuv_buf[y_end_pos + ((y/2)*cam_fmt_h) + (x/2) ] & (if x % 2 == 0 { 0xf0 } else { 0x0f} ); // bottom high 4 nibble
          let frame_v = 63; // frame_yuv_buf[y_end_pos + ((y/2)*cam_fmt_h) + (y*cam_fmt_h) + (x/2) ] & (if x % 2 == 0 { 0xf0 } else { 0x0f} ); // bottom low 4 nibble

          // Used conversion constants from https://stackoverflow.com/a/6959465

          // Normalize to between 0.0 and 1.0
          let y = (frame_y as f64) / 255.0;
          let u = (frame_u as f64) / 127.0;
          let v = (frame_v as f64) / 127.0;

          cam_rgb_buf[r_idx] = ((y + (1.139837398373983740*v) )*255.0) as u8;
          cam_rgb_buf[g_idx] = ((y - (0.3946517043589703515*u) - (0.5805986066674976801*v))*255.0) as u8;
          cam_rgb_buf[b_idx] = ((y + 2.032110091743119266*u)*255.0) as u8;

        }
      }

      // Process the BGR/RGB/whatevs pixels, drawing onto &mut embed_fb

      for y in 0..cam_fmt_h {
        for x in 0..cam_fmt_w {
          let fb_px_offset = ( ((y*cam_fmt_h) + x) * fb_bpp) as usize;

          let r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          embed_fb.set_pixel(
            embedded_graphics::geometry::Point { x: x as i32, y: y as i32 },
            embedded_graphics::pixelcolor::Bgr888::new(
              cam_rgb_buf[r_idx], cam_rgb_buf[g_idx], cam_rgb_buf[b_idx]
            )
          );
        }
      }


      Text::new("Hello Rust!", Point::new(EMBED_FB_W as i32 - 120, EMBED_FB_H as i32 - 40), font_style).draw(&mut embed_fb)?;


      // send to framebuffer!
      let embed_fb_data = embed_fb.data();
      for y in 0..fb_h {
        for x in 0..fb_w {

          let fb_px_offset = ( ((y*fb_w) + x) * fb_bpp) as usize;
          let embed_fb_px_offset = (((y*EMBED_FB_W) + x) * EMBED_FB_BPP) as usize;

          let embed_fb_r_idx = embed_fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let embed_fb_g_idx = embed_fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let embed_fb_b_idx = embed_fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

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
        }
      }


  }

  Ok(())
}









