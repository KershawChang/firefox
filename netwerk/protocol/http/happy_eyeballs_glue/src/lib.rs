use nserror::{nsresult, NS_ERROR_UNEXPECTED, NS_OK};
use nsstring::nsACString;
use thin_vec::ThinVec;
use std::net::{Ipv4Addr, Ipv6Addr};
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

#[repr(C)]
pub enum HEDnsRecordType {
    Https = 0,
    Aaaa = 1,
    A = 2,
}

#[repr(C)]
pub enum HETimerType {
    ResolutionDelay = 0,
    ConnectionAttemptDelay = 1,
    LastResortSynthesis = 2,
}

#[repr(C)]
pub enum HEProtocol {
    H3 = 0,
    H2 = 1,
    H1 = 2,
}

impl From<happy_eyeballs::DnsRecordType> for HEDnsRecordType {
    fn from(v: happy_eyeballs::DnsRecordType) -> Self {
        match v {
            happy_eyeballs::DnsRecordType::Https => Self::Https,
            happy_eyeballs::DnsRecordType::Aaaa => Self::Aaaa,
            happy_eyeballs::DnsRecordType::A => Self::A,
        }
    }
}

impl From<happy_eyeballs::Protocol> for HEProtocol {
    fn from(v: happy_eyeballs::Protocol) -> Self {
        match v {
            happy_eyeballs::Protocol::H3 => Self::H3,
            happy_eyeballs::Protocol::H2 => Self::H2,
            happy_eyeballs::Protocol::H1 => Self::H1,
        }
    }
}

impl From<happy_eyeballs::TimerType> for HETimerType {
    fn from(v: happy_eyeballs::TimerType) -> Self {
        match v {
            happy_eyeballs::TimerType::ResolutionDelay => Self::ResolutionDelay,
            happy_eyeballs::TimerType::ConnectionAttemptDelay => Self::ConnectionAttemptDelay,
            happy_eyeballs::TimerType::LastResortSynthesis => Self::LastResortSynthesis,
        }
    }
}

#[repr(C)]
pub enum HappyEyeballsInputKind {
    None = 0,
    Cancel = 1,
    DnsResponseA = 2,
    DnsResponseAaaa = 3,
}

#[repr(C)]
pub enum HappyEyeballsEvent {
    SendDnsQuery { record_type: HEDnsRecordType },
    Timer { timer_type: HETimerType, duration_ms: u64 },
    AttemptConnection { protocol: HEProtocol, port: u16 },
    CancelConnection { port: u16 },
    NoEvent,
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process(
    he: &mut HappyEyeballs,
    input_kind: HappyEyeballsInputKind,
    hostname: *const nsACString,
    addr_bytes: *const u8,
    addr_len: u32,
    ret_event: &mut HappyEyeballsEvent,
    data: &mut ThinVec<u8>,
) -> nsresult {
    let input = match input_kind {
        HappyEyeballsInputKind::None => None,
        HappyEyeballsInputKind::Cancel => Some(happy_eyeballs::Input::Cancel),
        HappyEyeballsInputKind::DnsResponseA | HappyEyeballsInputKind::DnsResponseAaaa => {
            if hostname.is_null() {
                return NS_ERROR_UNEXPECTED;
            }
            let host = unsafe { (&*hostname).to_utf8().to_string() };
            let name = happy_eyeballs::TargetName::from(host.as_str());
            if addr_len == 0 {
                // Empty, but still a valid (negative) response.
                let inner = match input_kind {
                    HappyEyeballsInputKind::DnsResponseA => happy_eyeballs::DnsResponseInner::A(Ok(Vec::new())),
                    HappyEyeballsInputKind::DnsResponseAaaa => happy_eyeballs::DnsResponseInner::Aaaa(Ok(Vec::new())),
                    _ => unreachable!(),
                };
                Some(happy_eyeballs::Input::DnsResponse(happy_eyeballs::DnsResponse { target_name: name, inner }))
            } else {
                if addr_bytes.is_null() {
                    return NS_ERROR_UNEXPECTED;
                }
                let slice = unsafe { std::slice::from_raw_parts(addr_bytes, addr_len as usize) };
                let inner = match input_kind {
                    HappyEyeballsInputKind::DnsResponseA => {
                        if slice.len() % 4 != 0 { return NS_ERROR_UNEXPECTED; }
                        let mut addrs = Vec::with_capacity(slice.len() / 4);
                        for chunk in slice.chunks_exact(4) {
                            let ipv4 = Ipv4Addr::new(chunk[0], chunk[1], chunk[2], chunk[3]);
                            addrs.push(ipv4);
                        }
                        happy_eyeballs::DnsResponseInner::A(Ok(addrs))
                    }
                    HappyEyeballsInputKind::DnsResponseAaaa => {
                        if slice.len() % 16 != 0 { return NS_ERROR_UNEXPECTED; }
                        let mut addrs = Vec::with_capacity(slice.len() / 16);
                        for chunk in slice.chunks_exact(16) {
                            let mut octs = [0u8; 16];
                            octs.copy_from_slice(chunk);
                            let ipv6 = Ipv6Addr::from(octs);
                            addrs.push(ipv6);
                        }
                        happy_eyeballs::DnsResponseInner::Aaaa(Ok(addrs))
                    }
                    _ => unreachable!(),
                };
                Some(happy_eyeballs::Input::DnsResponse(happy_eyeballs::DnsResponse { target_name: name, inner }))
            }
        }
    };

    let out = he.inner.process(input, std::time::Instant::now());
    match out {
        Some(happy_eyeballs::Output::SendDnsQuery { hostname: _hostname, record_type }) => {
            // Hostname is known by the caller; do not emit here.
            *ret_event = HappyEyeballsEvent::SendDnsQuery { record_type: record_type.into() };
        }
        Some(happy_eyeballs::Output::Timer { timer_type, duration }) => {
            let duration_ms = duration.as_millis();
            let Ok(duration_ms) = u64::try_from(duration_ms) else {
                return NS_ERROR_UNEXPECTED;
            };
            *ret_event = HappyEyeballsEvent::Timer { timer_type: timer_type.into(), duration_ms };
        }
        Some(happy_eyeballs::Output::AttemptConnection { endpoint }) => {
            let ip_str = endpoint.address.ip().to_string();
            data.extend_from_slice(ip_str.as_bytes());
            *ret_event = HappyEyeballsEvent::AttemptConnection {
                protocol: endpoint.protocol.into(),
                port: endpoint.address.port(),
            };
        }
        Some(happy_eyeballs::Output::CancelConnection(addr)) => {
            let ip_str = addr.ip().to_string();
            data.extend_from_slice(ip_str.as_bytes());
            *ret_event = HappyEyeballsEvent::CancelConnection { port: addr.port() };
        }
        None => {
            *ret_event = HappyEyeballsEvent::NoEvent;
        }
    }

    NS_OK
}
