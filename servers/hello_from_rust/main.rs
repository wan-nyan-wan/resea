#![no_std]
#![feature(asm)]
#![feature(global_asm)]
#![feature(lang_items)]
#![feature(alloc_error_handler)]
#![allow(clippy::missing_safety_doc)]

use core::alloc::Layout;
use core::panic;

#[lang = "eh_personality"]
#[no_mangle]
#[cfg(not(test))]
pub fn eh_personality() {
    loop {} // FIXME:
}

#[panic_handler]
#[no_mangle]
#[cfg(not(test))]
pub fn panic(_info: &panic::PanicInfo) -> ! {
    loop {} // FIXME:
}

#[alloc_error_handler]
#[cfg(not(test))]
fn alloc_error(_layout: Layout) -> ! {
    loop {} // FIXME:
}

/// Prints a warning message.
#[macro_export]
macro_rules! info {
    ($fmt:expr) => {
//        $crate::println!(concat!("\x1b[0;94m", "[{}] ", $fmt, "\x1b[0m"), $crate::program_name());
    };
    ($fmt:expr, $($arg:tt)*) => {
//        $crate::println!(concat!("\x1b[0;94m", "[{}] ", $fmt, "\x1b[0m"), $crate::program_name(), $($arg)*);
    };
}

#[no_mangle]
pub fn main() {

    info!("Hello, World from Rust!");
}
