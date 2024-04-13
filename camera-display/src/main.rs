


use v4l::buffer::Type;
use v4l::io::mmap::Stream;
use v4l::io::traits::CaptureStream;
use v4l::video::Capture;
use v4l::Device;
use v4l::FourCC;


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

  let img_fmt_h = fmt.height as usize;
  let img_fmt_w = fmt.width as usize;
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
  let mut stream = Stream::with_buffers(&mut dev, Type::VideoCapture, 4)?;

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
  let mut img_buf: Vec<u8> = Vec::with_capacity(img_fmt_h * img_fmt_w * fb_bpp);
  img_buf.resize(img_fmt_h * img_fmt_w * fb_bpp, 0); // zero buffer


  let mut loop_i = 0;
  loop {
      loop_i += 1;
      if loop_i > 1000 {
        loop_i = 0;
      }

      let (frame_yuv_buf, meta) = stream.next()?;
      println!(
          "Buffer size: {}, seq: {}, timestamp: {}",
          frame_yuv_buf.len(),
          meta.sequence,
          meta.timestamp
      );

      // To process the captured data, you can pass it somewhere else.
      // If you want to modify the data or extend its lifetime, you have to
      // copy it. This is a best-effort tradeoff solution that allows for
      // zero-copy readers while enforcing a full clone of the data for
      // writers.

      let mut fb_mem = match fb.map() {
        Ok(m) => m,
        Err(e) => {
          println!("fb.map() e = {:?}", e);
          return Ok(());
        }
      };

      // Decode YCbCr 4:2:2 pixels into BGR pixels

      for y in 0..img_fmt_h {
        for x in 0..img_fmt_w {
          let fb_px_offset = ( ((y*img_fmt_h) + x) * fb_bpp) as usize;
          /*if fb_px_offset+fb_bpp > img_buf.len() {
            continue;
          }*/

          let r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          // TODO use this as pixel offset calculation reference - https://i.stack.imgur.com/Vprp4.png

          let camera_px_offset = (((y*img_fmt_h) + x) * img_bpp) as usize;
          /*if camera_px_offset+img_bpp > frame_yuv_buf.len() {
            continue;
          }*/

          let frame_y = frame_yuv_buf[((y*img_fmt_h) + x) as usize]; // top 8 bits
          let y_end_pos = (img_fmt_h*img_fmt_w) as usize;
          let frame_u = 63; // frame_yuv_buf[y_end_pos + ((y/2)*img_fmt_h) + (x/2) ] & (if x % 2 == 0 { 0xf0 } else { 0x0f} ); // bottom high 4 nibble
          let frame_v = 63; // frame_yuv_buf[y_end_pos + ((y/2)*img_fmt_h) + (y*img_fmt_h) + (x/2) ] & (if x % 2 == 0 { 0xf0 } else { 0x0f} ); // bottom low 4 nibble

          // Used conversion constants from https://stackoverflow.com/a/6959465

          // Normalize to between 0.0 and 1.0
          let y = (frame_y as f64) / 255.0;
          let u = (frame_u as f64) / 127.0;
          let v = (frame_v as f64) / 127.0;

          img_buf[r_idx] = ((y + (1.139837398373983740*v) )*255.0) as u8;
          img_buf[g_idx] = ((y - (0.3946517043589703515*u) - (0.5805986066674976801*v))*255.0) as u8;
          img_buf[b_idx] = ((y + 2.032110091743119266*u)*255.0) as u8;

        }
      }

      // Process the BGR/RGB/whatevs pixels



      // send to framebuffer!
      for y in 0..img_fmt_h {
        for x in 0..img_fmt_w {
          let fb_px_offset = ( ((y*img_fmt_h) + x) * fb_bpp) as usize;
          if fb_px_offset+fb_bpp >= frame_yuv_buf.len() || fb_px_offset+fb_bpp > fb_mem.len() || fb_px_offset+fb_bpp > img_buf.len() {
            continue;
          }

          let r_idx = fb_px_offset + (fb_pxlyt.red.offset / 8) as usize;
          let g_idx = fb_px_offset + (fb_pxlyt.green.offset / 8) as usize;
          let b_idx = fb_px_offset + (fb_pxlyt.blue.offset / 8) as usize;

          fb_mem[r_idx] = img_buf[r_idx];
          fb_mem[g_idx] = img_buf[g_idx];
          fb_mem[b_idx] = img_buf[b_idx];

        }
      }



  }

  Ok(())
}









