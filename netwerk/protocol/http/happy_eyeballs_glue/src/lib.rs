use nserror::{nsresult, NS_ERROR_INVALID_ARG, NS_ERROR_UNEXPECTED, NS_OK};
use nsstring::{nsACString, nsCString};
use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
use std::ptr;
use std::time::Instant;
use thin_vec::ThinVec;
use xpcom::{AtomicRefcnt, RefCounted, RefPtr};

#[cfg(not(windows))]
use libc::{AF_INET, AF_INET6};
#[cfg(windows)]
use winapi::{
    shared::ws2def::{AF_INET, AF_INET6},
};

// Opaque interface to mozilla::net::NetAddr defined in DNS.h
#[repr(C)]
pub union NetAddr {
    _private: [u8; 0],
}

extern "C" {
    fn moz_netaddr_get_family(arg: *const NetAddr) -> u16;
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
    fn process_dns_response_a(&mut self, id: u64, addrs: &ThinVec<NetAddr>) -> nsresult {
        let mut out = Vec::with_capacity(addrs.len());
        for na in addrs.iter() {
            let family = i32::from(unsafe {
                moz_netaddr_get_family((na as *const NetAddr).cast())
            });
            if family != AF_INET {
                return NS_ERROR_UNEXPECTED;
            }
            let ip_be =
                unsafe { moz_netaddr_get_network_order_ip((na as *const NetAddr).cast()) };
            let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
            out.push(ipv4);
        }

        let result = happy_eyeballs::DnsResult::A(Ok(out));
        let input = happy_eyeballs::Input::DnsResult {
            id: id.into(),
            result,
        };
        self.inner.process_input(input, Instant::now());

        NS_OK
    }

    fn process_dns_response_aaaa(&mut self, id: u64, addrs: &ThinVec<NetAddr>) -> nsresult {
        let mut out = Vec::with_capacity(addrs.len());
        for na in addrs.iter() {
            let family = i32::from(unsafe {
                moz_netaddr_get_family((na as *const NetAddr).cast())
            });
            if family != AF_INET6 {
                return NS_ERROR_UNEXPECTED;
            }
            let p = unsafe { moz_netaddr_get_ipv6((na as *const NetAddr).cast()) };
            let octs: [u8; 16] =
                unsafe { std::slice::from_raw_parts(p, 16).try_into().unwrap() };
            let ipv6 = Ipv6Addr::from(octs);
            out.push(ipv6);
        }

        let result = happy_eyeballs::DnsResult::Aaaa(Ok(out));
        let input = happy_eyeballs::Input::DnsResult {
            id: id.into(),
            result,
        };
        self.inner.process_input(input, Instant::now());

        NS_OK
    }

    fn process_dns_response_https(
        &mut self,
        id: u64,
        service_infos: &ThinVec<ServiceInfoFFI>,
    ) -> nsresult {
        let mut parsed_infos = Vec::new();

        for svc_info in service_infos {
                let target_str = svc_info.target_name.to_utf8();
                let target = if target_str.is_empty() {
                    todo!()
                } else {
                    happy_eyeballs::TargetName::from(target_str.as_ref())
                };

                let mut alpn_set = std::collections::HashSet::new();
                for protocol in &svc_info.alpn_protocols {
                    alpn_set.insert((*protocol).into());
                }

                let ech = if svc_info.ech_config.is_empty() {
                    None
                } else {
                    Some(svc_info.ech_config.to_vec())
                };

                let mut ipv4_vec = Vec::new();
                for na in &svc_info.ipv4_hints {
                    let family = i32::from(unsafe {
                        moz_netaddr_get_family((na as *const NetAddr).cast())
                    });
                    debug_assert_eq!(family, AF_INET, "Expected IPv4 address in IPv4 hints");
                    if family != AF_INET {
                        return NS_ERROR_UNEXPECTED;
                    }
                    let ip_be = unsafe {
                        moz_netaddr_get_network_order_ip((na as *const NetAddr).cast())
                    };
                    let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
                    ipv4_vec.push(ipv4);
                }

                let mut ipv6_vec = Vec::new();
                for na in &svc_info.ipv6_hints {
                    let family = i32::from(unsafe {
                        moz_netaddr_get_family((na as *const NetAddr).cast())
                    });
                    debug_assert_eq!(family, AF_INET6, "Expected IPv6 address in IPv6 hints");
                    if family != AF_INET6 {
                        return NS_ERROR_UNEXPECTED;
                    }
                    let p = unsafe { moz_netaddr_get_ipv6((na as *const NetAddr).cast()) };
                    let octs: [u8; 16] =
                        unsafe { std::slice::from_raw_parts(p, 16).try_into().unwrap() };
                    let ipv6 = Ipv6Addr::from(octs);
                    ipv6_vec.push(ipv6);
                }

                parsed_infos.push(happy_eyeballs::ServiceInfo {
                    priority: svc_info.priority,
                    target_name: target,
                    alpn_protocols: alpn_set,
                    ech_config: ech,
                    ipv4_hints: ipv4_vec,
                    ipv6_hints: ipv6_vec,
                });
        }

        let result = happy_eyeballs::DnsResult::Https(Ok(parsed_infos));
        let input = happy_eyeballs::Input::DnsResult {
            id: id.into(),
            result,
        };
        self.inner.process_input(input, Instant::now());

        NS_OK
    }

    fn process_connection_result(&mut self, id: u64, status: nsresult) -> nsresult {
        let result = if status == NS_OK {
            Ok(())
        } else {
            Err(format!("connection failed: 0x{:08x}", status.0))
        };

        let input = happy_eyeballs::Input::ConnectionResult {
            id: id.into(),
            result,
        };
        self.inner.process_input(input, Instant::now());

        NS_OK
    }

    // TODO: Split data into single purpose thinvecs
    fn process_output(&mut self, ret_event: &mut Output, data: &mut ThinVec<u8>) -> nsresult {
        let out = self.inner.process_output(std::time::Instant::now());
        data.clear();
        match out {
            Some(happy_eyeballs::Output::SendDnsQuery {
                id,
                hostname: _hostname,
                record_type,
            }) => {
                *ret_event = Output::SendDnsQuery {
                    id: id.into(),
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
            Some(happy_eyeballs::Output::AttemptConnection { id, endpoint }) => {
                let addr_str = endpoint.address.ip().to_string();
                let addr_len = addr_str.len() as u32;
                data.extend_from_slice(addr_str.as_bytes());
                let ech_config_len = if let Some(ref ech) = endpoint.ech_config {
                    data.extend_from_slice(ech);
                    ech.len() as u32
                } else {
                    0
                };
                *ret_event = Output::AttemptConnection {
                    id: id.into(),
                    protocol: endpoint.protocol.into(),
                    port: endpoint.address.port(),
                    addr_len,
                    ech_config_len,
                };
            }
            Some(happy_eyeballs::Output::CancelConnection(addr)) => {
                let addr_str = addr.ip().to_string();
                data.extend_from_slice(addr_str.as_bytes());
                *ret_event = Output::CancelConnection { port: addr.port() };
            }
            Some(happy_eyeballs::Output::Succeeded) => {
                *ret_event = Output::Succeeded;
            }
            Some(happy_eyeballs::Output::Failed) => {
                *ret_event = Output::Failed;
            }
            None => {
                *ret_event = Output::None;
            }
        }

        NS_OK
    }
}

// TODO: Move up. New is always at the top.
#[no_mangle]
pub extern "C" fn happy_eyeballs_new(
    result: &mut *const HappyEyeballs,
    origin: *const nsACString,
    port: u16,
    alt_svc: &ThinVec<AltSvc>,
) -> nsresult {
    *result = ptr::null_mut();

    if origin.is_null() {
        return NS_ERROR_INVALID_ARG;
    }

    let origin_str = unsafe { (&*origin).to_utf8().to_string() };

    let alt_svc_vec: Vec<_> = alt_svc
        .iter()
        .map(|a| happy_eyeballs::AltSvc {
            host: None,
            port: None,
            protocol: a.protocol.into(),
        })
        .collect();

    let network_config = happy_eyeballs::NetworkConfig {
        alt_svc: alt_svc_vec,
        ..Default::default()
    };

    let happy_eyeballs = match happy_eyeballs::HappyEyeballs::new_with_network_config(
        origin_str.as_str(),
        port,
        network_config,
    ) {
        Ok(he) => Box::into_raw(Box::new(HappyEyeballs {
            refcnt: unsafe { AtomicRefcnt::new() },
            inner: he,
        })),
        Err(_) => return NS_ERROR_UNEXPECTED,
    };

    match unsafe { RefPtr::from_raw(happy_eyeballs) } {
        Some(ptr) => {
            ptr.forget(result);
            NS_OK
        }
        None => NS_ERROR_UNEXPECTED,
    }
}

// TODO: Expose ip and port.
#[repr(C)]
pub struct AltSvc {
    pub protocol: Protocol,
}

#[repr(C)]
pub enum DnsRecordType {
    Https = 0,
    Aaaa = 1,
    A = 2,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum Protocol {
    H3 = 0,
    H2 = 1,
    H1 = 2,
}

#[repr(C)]
pub enum ConnectionAttemptProtocols {
    H3 = 0,
    H2OrH1 = 1,
    H2 = 2,
    H1 = 3,
}

impl From<Protocol> for happy_eyeballs::Protocol {
    fn from(v: Protocol) -> Self {
        match v {
            Protocol::H3 => Self::H3,
            Protocol::H2 => Self::H2,
            Protocol::H1 => Self::H1,
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

impl From<happy_eyeballs::DnsRecordType> for DnsRecordType {
    fn from(v: happy_eyeballs::DnsRecordType) -> Self {
        match v {
            happy_eyeballs::DnsRecordType::Https => Self::Https,
            happy_eyeballs::DnsRecordType::Aaaa => Self::Aaaa,
            happy_eyeballs::DnsRecordType::A => Self::A,
        }
    }
}

impl From<happy_eyeballs::ConnectionAttemptProtocols> for ConnectionAttemptProtocols {
    fn from(v: happy_eyeballs::ConnectionAttemptProtocols) -> Self {
        match v {
            happy_eyeballs::ConnectionAttemptProtocols::H3 => Self::H3,
            happy_eyeballs::ConnectionAttemptProtocols::H2OrH1 => Self::H2OrH1,
            happy_eyeballs::ConnectionAttemptProtocols::H2 => Self::H2,
            happy_eyeballs::ConnectionAttemptProtocols::H1 => Self::H1,
        }
    }
}

impl From<ConnectionAttemptProtocols> for happy_eyeballs::ConnectionAttemptProtocols {
    fn from(v: ConnectionAttemptProtocols) -> Self {
        match v {
            ConnectionAttemptProtocols::H3 => Self::H3,
            ConnectionAttemptProtocols::H2OrH1 => Self::H2OrH1,
            ConnectionAttemptProtocols::H2 => Self::H2,
            ConnectionAttemptProtocols::H1 => Self::H1,
        }
    }
}

impl From<happy_eyeballs::Protocol> for ConnectionAttemptProtocols {
    fn from(v: happy_eyeballs::Protocol) -> Self {
        match v {
            happy_eyeballs::Protocol::H3 => Self::H3,
            happy_eyeballs::Protocol::H2 => Self::H2,
            happy_eyeballs::Protocol::H1 => Self::H1,
        }
    }
}

#[repr(C)]
pub struct ServiceInfoFFI {
    pub priority: u16,
    pub target_name: nsCString,
    pub alpn_protocols: ThinVec<Protocol>,
    pub ech_config: ThinVec<u8>,
    pub ipv4_hints: ThinVec<NetAddr>,
    pub ipv6_hints: ThinVec<NetAddr>,
}

#[repr(C)]
pub enum Output {
    SendDnsQuery { id: u64, record_type: DnsRecordType },
    Timer { duration_ms: u64 },
    AttemptConnection { id: u64, protocol: ConnectionAttemptProtocols, port: u16, addr_len: u32, ech_config_len: u32 },
    CancelConnection { port: u16 },
    Succeeded,
    Failed,
    None,
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_a(
    he: &mut HappyEyeballs,
    id: u64,
    addrs: &ThinVec<NetAddr>,
) -> nsresult {
    he.process_dns_response_a(id, addrs)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_aaaa(
    he: &mut HappyEyeballs,
    id: u64,
    addrs: &ThinVec<NetAddr>,
) -> nsresult {
    he.process_dns_response_aaaa(id, addrs)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_https(
    he: &mut HappyEyeballs,
    id: u64,
    service_infos: &ThinVec<ServiceInfoFFI>,
) -> nsresult {
    he.process_dns_response_https(id, service_infos)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_connection_result(
    he: &mut HappyEyeballs,
    id: u64,
    status: nsresult,
) -> nsresult {
    he.process_connection_result(id, status)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_output(
    he: &mut HappyEyeballs,
    ret_event: &mut Output,
    data: &mut ThinVec<u8>,
) -> nsresult {
    he.process_output(ret_event, data)
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
