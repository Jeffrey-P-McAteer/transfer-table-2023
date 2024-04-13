


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
  fmt.width = 1280;
  fmt.height = 720;
  fmt.fourcc = FourCC::new(b"YUYV");
  let fmt = dev.set_format(&fmt)?;

  // The actual format chosen by the device driver may differ from what we
  // requested! Print it out to get an idea of what is actually used now.
  println!("Format in use:\n{}", fmt);

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

  // Open framebuffer for output
  let fb = match linuxfb::Framebuffer::new("/dev/fb0") {
    Ok(f) => f,
    Err(e) => {
        println!("linuxfb::Framebuffer::new() e = {:?}", e);
        return Ok(());
    }
  };

  println!("FB Size in pixels: {:?}", fb.get_size());

  println!("FB Bytes per pixel: {:?}", fb.get_bytes_per_pixel());

  println!("FB Physical size in mm: {:?}", fb.get_physical_size());


  let mut loop_i = 0;
  loop {
      loop_i += 1;
      if loop_i > 1000 {
        loop_i = 0;
      }

      let (buf, meta) = stream.next()?;
      println!(
          "Buffer size: {}, seq: {}, timestamp: {}",
          buf.len(),
          meta.sequence,
          meta.timestamp
      );

      // To process the captured data, you can pass it somewhere else.
      // If you want to modify the data or extend its lifetime, you have to
      // copy it. This is a best-effort tradeoff solution that allows for
      // zero-copy readers while enforcing a full clone of the data for
      // writers.

      let mut data = match fb.map() {
        Ok(m) => m,
        Err(e) => {
          println!("fb.map() e = {:?}", e);
          return Ok(());
        }
      };

      // Make everything black:
      for i in 0..data.len() {
        data[i] = 0;
      }

      std::thread::sleep(std::time::Duration::from_millis(50));

      // Make everything white:
      for i in 0..data.len() {
        data[i] = 0xFF;
      }

      if loop_i > 50 {
        break;
      }

  }

  Ok(())
}









