use nserror::{nsresult, NS_ERROR_INVALID_ARG, NS_ERROR_UNEXPECTED, NS_OK};
use nsstring::nsACString;
use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
use std::ptr;
use thin_vec::ThinVec;
use xpcom::{AtomicRefcnt, RefCounted, RefPtr};

// Opaque interface to mozilla::net::NetAddr defined in DNS.h
#[repr(C)]
pub union NetAddr {
    _private: [u8; 0],
}

extern "C" {
    fn moz_netaddr_get_network_order_ip(arg: *const NetAddr) -> u32;
    fn moz_netaddr_get_ipv6(arg: *const NetAddr) -> *const u8;
    fn moz_netaddr_get_network_order_port(arg: *const NetAddr) -> u16;
}

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
        addrs: *const NetAddr,
        addrs_len: u32,
        ret_event: &mut Output,
        data: &mut ThinVec<u8>,
    ) -> nsresult {
        let input = match input_kind {
            InputKind::None => None,
            InputKind::ConnectionResult => {
                if addrs.is_null() || addrs_len != 1 {
                    return NS_ERROR_UNEXPECTED;
                }
                let netaddr = unsafe { &*addrs };
                let port = u16::from_be(unsafe { moz_netaddr_get_network_order_port(netaddr) });

                let ipv6_ptr = unsafe { moz_netaddr_get_ipv6(netaddr) };
                let address = if !ipv6_ptr.is_null() {
                    let octs: [u8; 16] = unsafe {
                        std::slice::from_raw_parts(ipv6_ptr, 16)
                            .try_into()
                            .unwrap()
                    };
                    let ipv6 = Ipv6Addr::from(octs);
                    SocketAddr::from((ipv6, port))
                } else {
                    let ip_be = unsafe { moz_netaddr_get_network_order_ip(netaddr) };
                    let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
                    SocketAddr::from((ipv4, port))
                };

                let result = if data.is_empty() {
                    Ok(())
                } else {
                    let error_msg = match std::str::from_utf8(data.as_slice()) {
                        Ok(s) => s.to_string(),
                        Err(_) => String::from("connection failed"),
                    };
                    Err(error_msg)
                };
                Some(happy_eyeballs::Input::ConnectionResult { address, result })
            }
            InputKind::DnsResponseA | InputKind::DnsResponseAaaa => {
                if hostname.is_null() {
                    return NS_ERROR_UNEXPECTED;
                }
                let host = unsafe { (&*hostname).to_utf8().to_string() };
                let name = happy_eyeballs::TargetName::from(host.as_str());
                if addrs_len == 0 {
                    let inner = match input_kind {
                        InputKind::DnsResponseA => {
                            happy_eyeballs::DnsResultInner::A(Ok(Vec::new()))
                        }
                        InputKind::DnsResponseAaaa => {
                            happy_eyeballs::DnsResultInner::Aaaa(Ok(Vec::new()))
                        }
                        _ => unreachable!(),
                    };
                    Some(happy_eyeballs::Input::DnsResult(
                        happy_eyeballs::DnsResult {
                            target_name: name,
                            inner,
                        },
                    ))
                } else {
                    if addrs.is_null() {
                        return NS_ERROR_UNEXPECTED;
                    }
                    let slice = unsafe { std::slice::from_raw_parts(addrs, addrs_len as usize) };
                    let inner = match input_kind {
                        InputKind::DnsResponseA => {
                            // TODO: Sane?
                            let mut out = Vec::with_capacity(slice.len());
                            for na in slice.iter() {
                                let ip_be = unsafe {
                                    moz_netaddr_get_network_order_ip(
                                        (na as *const NetAddr).cast(),
                                    )
                                };
                                let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
                                out.push(ipv4);
                            }
                            happy_eyeballs::DnsResultInner::A(Ok(out))
                        }
                        InputKind::DnsResponseAaaa => {
                            // TODO: Sane?
                            let mut out = Vec::with_capacity(slice.len());
                            for na in slice.iter() {
                                let p = unsafe { moz_netaddr_get_ipv6((na as *const NetAddr).cast()) };
                                if p.is_null() {
                                    return NS_ERROR_UNEXPECTED;
                                }
                                let octs: [u8; 16] = unsafe {
                                    std::slice::from_raw_parts(p, 16)
                                        .try_into()
                                        .unwrap()
                                };
                                let ipv6 = Ipv6Addr::from(octs);
                                out.push(ipv6);
                            }
                            happy_eyeballs::DnsResultInner::Aaaa(Ok(out))
                        }
                        _ => unreachable!(),
                    };
                    Some(happy_eyeballs::Input::DnsResult(
                        happy_eyeballs::DnsResult {
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
                let addr_str = endpoint.address.to_string();
                data.extend_from_slice(addr_str.as_bytes());
                *ret_event = Output::AttemptConnection {
                    protocol: endpoint.protocol.into(),
                    port: endpoint.address.port(),
                };
            }
            Some(happy_eyeballs::Output::CancelConnection(addr)) => {
                let addr_str = addr.to_string();
                data.extend_from_slice(addr_str.as_bytes());
                *ret_event = Output::CancelConnection {
                    port: addr.port(),
                };
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

    if origin.is_null() {
        return NS_ERROR_INVALID_ARG;
    }

    let origin_str = unsafe { (&*origin).to_utf8().to_string() };

    let happy_eyeballs = match HappyEyeballs::new(origin_str.as_str(), port) {
        Ok(he) => Box::into_raw(Box::new(he)),
        Err(_) => return NS_ERROR_UNEXPECTED,
    };

    match unsafe { RefPtr::from_raw(happy_eyeballs) } {
        Some(ptr) => {
            unsafe { ptr.forget(result) };
            NS_OK
        }
        None => NS_ERROR_UNEXPECTED,
    }
}

#[repr(C)]
pub enum DnsRecordType {
    Https = 0,
    Aaaa = 1,
    A = 2,
}

#[repr(C)]
pub enum ProtocolCombination {
    H3 = 0,
    H2OrH1 = 1,
    H2 = 2,
    H1 = 3,
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

impl From<happy_eyeballs::ProtocolCombination> for ProtocolCombination {
    fn from(v: happy_eyeballs::ProtocolCombination) -> Self {
        match v {
            happy_eyeballs::ProtocolCombination::H3 => Self::H3,
            happy_eyeballs::ProtocolCombination::H2OrH1 => Self::H2OrH1,
            happy_eyeballs::ProtocolCombination::H2 => Self::H2,
            happy_eyeballs::ProtocolCombination::H1 => Self::H1,
        }
    }
}

impl From<happy_eyeballs::Protocol> for ProtocolCombination {
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
    DnsResponseA = 2,
    DnsResponseAaaa = 3,
    ConnectionResult = 4,
}

#[repr(C)]
pub enum Output {
    SendDnsQuery { record_type: DnsRecordType },
    Timer { duration_ms: u64 },
    AttemptConnection { protocol: ProtocolCombination, port: u16 },
    CancelConnection { port: u16 },
    None,
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process(
    he: &mut HappyEyeballs,
    input_kind: InputKind,
    hostname: *const nsACString,
    addrs: *const NetAddr,
    addrs_len: u32,
    ret_event: &mut Output,
    data: &mut ThinVec<u8>,
) -> nsresult {
    he.process(input_kind, hostname, addrs, addrs_len, ret_event, data)
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
