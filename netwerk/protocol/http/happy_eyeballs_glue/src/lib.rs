use nserror::{nsresult, NS_ERROR_UNEXPECTED, NS_OK};
use nsstring::nsACString;
use std::net::{Ipv4Addr, Ipv6Addr};
use std::ptr;
use thin_vec::ThinVec;
use xpcom::{AtomicRefcnt, RefCounted};

#[repr(C)]
pub struct HappyEyeballs {
    refcnt: AtomicRefcnt,
    inner: happy_eyeballs::HappyEyeballs,
}

impl HappyEyeballs {
    fn new(origin: &str, port: u16) -> Result<Self, happy_eyeballs::ConstructorError> {
        tracing::debug!(
            "HappyEyeballs::new called with origin: {}, port: {}",
            origin,
            port
        );
        Ok(Self {
            refcnt: unsafe { AtomicRefcnt::new() },
            inner: happy_eyeballs::HappyEyeballs::new(origin, port)?,
        })
    }

    fn process(
        &mut self,
        input_kind: InputKind,
        hostname: *const nsACString,
        addr_bytes: *const u8,
        addr_len: u32,
        ret_event: &mut Output,
        data: &mut ThinVec<u8>,
    ) -> nsresult {
        let input = match input_kind {
            InputKind::None => None,
            InputKind::Cancel => Some(happy_eyeballs::Input::Cancel),
            InputKind::DnsResponseA | InputKind::DnsResponseAaaa => {
                if hostname.is_null() {
                    return NS_ERROR_UNEXPECTED;
                }
                let host = unsafe { (&*hostname).to_utf8().to_string() };
                let name = happy_eyeballs::TargetName::from(host.as_str());
                if addr_len == 0 {
                    let inner = match input_kind {
                        InputKind::DnsResponseA => {
                            happy_eyeballs::DnsResponseInner::A(Ok(Vec::new()))
                        }
                        InputKind::DnsResponseAaaa => {
                            happy_eyeballs::DnsResponseInner::Aaaa(Ok(Vec::new()))
                        }
                        _ => unreachable!(),
                    };
                    Some(happy_eyeballs::Input::DnsResponse(
                        happy_eyeballs::DnsResponse {
                            target_name: name,
                            inner,
                        },
                    ))
                } else {
                    if addr_bytes.is_null() {
                        return NS_ERROR_UNEXPECTED;
                    }
                    let slice =
                        unsafe { std::slice::from_raw_parts(addr_bytes, addr_len as usize) };
                    let inner = match input_kind {
                        InputKind::DnsResponseA => {
                            if slice.len() % 4 != 0 {
                                return NS_ERROR_UNEXPECTED;
                            }
                            let mut addrs = Vec::with_capacity(slice.len() / 4);
                            for chunk in slice.chunks_exact(4) {
                                let ipv4 = Ipv4Addr::new(chunk[0], chunk[1], chunk[2], chunk[3]);
                                addrs.push(ipv4);
                            }
                            happy_eyeballs::DnsResponseInner::A(Ok(addrs))
                        }
                        InputKind::DnsResponseAaaa => {
                            if slice.len() % 16 != 0 {
                                return NS_ERROR_UNEXPECTED;
                            }
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
                    Some(happy_eyeballs::Input::DnsResponse(
                        happy_eyeballs::DnsResponse {
                            target_name: name,
                            inner,
                        },
                    ))
                }
            }
        };

        let out = self.inner.process(input, std::time::Instant::now());
        match out {
            Some(happy_eyeballs::Output::SendDnsQuery {
                hostname: _hostname,
                record_type,
            }) => {
                *ret_event = Output::SendDnsQuery {
                    record_type: record_type.into(),
                };
            }
            Some(happy_eyeballs::Output::Timer { duration, .. }) => {
                *ret_event = Output::Timer {
                    duration_ms: duration.as_millis().try_into().unwrap_or_else(|_| {
                        debug_assert!(false, "duration > u64::MAX");
                        u64::MAX
                    }),
                };
            }
            Some(happy_eyeballs::Output::AttemptConnection { endpoint }) => {
                let ip_str = endpoint.address.ip().to_string();
                data.extend_from_slice(ip_str.as_bytes());
                *ret_event = Output::AttemptConnection {
                    protocol: endpoint.protocol.into(),
                    port: endpoint.address.port(),
                };
            }
            Some(happy_eyeballs::Output::CancelConnection(_addr)) => {
                unimplemented!();
            }
            None => {
                *ret_event = Output::None;
            }
        }

        NS_OK
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
    let happy_eyeballs = match HappyEyeballs::new(origin_str.as_str(), port) {
        Ok(he) => he,
        Err(_) => return NS_ERROR_UNEXPECTED,
    };
    unsafe {
        xpcom::RefPtr::from_raw(Box::into_raw(Box::new(happy_eyeballs)))
            .unwrap()
            .forget(result)
    };
    NS_OK
}

#[repr(C)]
pub enum DnsRecordType {
    Https = 0,
    Aaaa = 1,
    A = 2,
}

#[repr(C)]
pub enum Protocol {
    H3 = 0,
    H2 = 1,
    H1 = 2,
}

impl From<happy_eyeballs::DnsRecordType> for DnsRecordType {
    fn from(v: happy_eyeballs::DnsRecordType) -> Self {
        match v {
            happy_eyeballs::DnsRecordType::Https => Self::Https,
            happy_eyeballs::DnsRecordType::Aaaa => Self::Aaaa,
            happy_eyeballs::DnsRecordType::A => Self::A,
        }
    }
}

impl From<happy_eyeballs::Protocol> for Protocol {
    fn from(v: happy_eyeballs::Protocol) -> Self {
        match v {
            happy_eyeballs::Protocol::H3 => Self::H3,
            happy_eyeballs::Protocol::H2 => Self::H2,
            happy_eyeballs::Protocol::H1 => Self::H1,
        }
    }
}

#[repr(C)]
pub enum InputKind {
    None = 0,
    Cancel = 1,
    DnsResponseA = 2,
    DnsResponseAaaa = 3,
}

#[repr(C)]
pub enum Output {
    SendDnsQuery { record_type: DnsRecordType },
    Timer { duration_ms: u64 },
    AttemptConnection { protocol: Protocol, port: u16 },
    None,
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process(
    he: &mut HappyEyeballs,
    input_kind: InputKind,
    hostname: *const nsACString,
    addr_bytes: *const u8,
    addr_len: u32,
    ret_event: &mut Output,
    data: &mut ThinVec<u8>,
) -> nsresult {
    he.process(input_kind, hostname, addr_bytes, addr_len, ret_event, data)
}

#[no_mangle]
pub unsafe extern "C" fn happy_eyeballs_release(happy_eyeballs: &HappyEyeballs) {
    let rc = happy_eyeballs.refcnt.dec();
    if rc == 0 {
        drop(Box::from_raw(ptr::from_ref(happy_eyeballs).cast_mut()));
    }
}

#[no_mangle]
pub unsafe extern "C" fn happy_eyeballs_addref(happy_eyeballs: &HappyEyeballs) {
    happy_eyeballs.refcnt.inc();
}

// xpcom::RefPtr support
unsafe impl RefCounted for HappyEyeballs {
    unsafe fn addref(&self) {
        happy_eyeballs_addref(self);
    }
    unsafe fn release(&self) {
        happy_eyeballs_release(self);
    }
}
