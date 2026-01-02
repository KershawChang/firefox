use nserror::{nsresult, NS_OK};
use nsstring::nsACString;
use std::ptr;
use xpcom::{AtomicRefcnt, RefCounted};

#[repr(C)]
pub struct HappyEyeballs {
    refcnt: AtomicRefcnt,
    inner: happy_eyeballs::HappyEyeballs,
}

impl HappyEyeballs {
    pub fn new(origin: String, port: u16) -> Self {
        tracing::debug!(
            "HappyEyeballs::new called with origin: {}, port: {}",
            origin,
            port
        );
        Self {
            refcnt: unsafe { AtomicRefcnt::new() },
            inner: happy_eyeballs::HappyEyeballs::new(origin, port),
        }
    }
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_new(
    result: &mut *const HappyEyeballs,
    origin: *const nsACString,
    port: u16,
) -> nsresult {
    *result = ptr::null_mut();
    let origin_str = unsafe {
        if origin.is_null() {
            String::new()
        } else {
            (&*origin).to_utf8().to_string()
        }
    };
    unsafe {
        xpcom::RefPtr::from_raw(Box::into_raw(Box::new(HappyEyeballs::new(
            origin_str, port,
        ))))
        .unwrap()
        .forget(result)
    };
    NS_OK
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_release(ptr: *const HappyEyeballs) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let obj = &*ptr;
        let rc = obj.refcnt.dec();
        if rc == 0 {
            drop(Box::from_raw(ptr as *mut HappyEyeballs));
        }
    }
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_addref(ptr: *const HappyEyeballs) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        let obj = &*ptr;
        obj.refcnt.inc();
    }
}

// xpcom::RefPtr support
unsafe impl RefCounted for HappyEyeballs {
    unsafe fn addref(&self) {
        self.refcnt.inc();
    }
    unsafe fn release(&self) {
        let rc = self.refcnt.dec();
        if rc == 0 {
            drop(Box::from_raw(self as *const _ as *mut HappyEyeballs));
        }
    }
}
