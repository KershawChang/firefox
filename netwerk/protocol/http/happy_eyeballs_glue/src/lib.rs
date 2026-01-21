use nserror::{nsresult, NS_ERROR_INVALID_ARG, NS_ERROR_UNEXPECTED, NS_OK};
use nsstring::nsACString;
use std::net::{Ipv4Addr, Ipv6Addr, SocketAddr};
use std::ptr;
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
    fn process_dns_response_a(
        &mut self,
        hostname: *const nsACString,
        addrs: *const NetAddr,
        addrs_len: u32,
    ) -> nsresult {
        if hostname.is_null() {
            return NS_ERROR_UNEXPECTED;
        }
        let host = unsafe { (&*hostname).to_utf8().to_string() };
        let name = happy_eyeballs::TargetName::from(host.as_str());

        let inner = if addrs_len == 0 {
            happy_eyeballs::DnsResultInner::A(Ok(Vec::new()))
        } else {
            if addrs.is_null() {
                return NS_ERROR_UNEXPECTED;
            }
            let slice = unsafe { std::slice::from_raw_parts(addrs, addrs_len as usize) };
            let mut out = Vec::with_capacity(slice.len());
            for na in slice.iter() {
                let family = i32::from(unsafe { moz_netaddr_get_family((na as *const NetAddr).cast()) });
                debug_assert_eq!(family, AF_INET, "Expected IPv4 address in A record response");
                if family != AF_INET {
                    return NS_ERROR_UNEXPECTED;
                }
                let ip_be =
                    unsafe { moz_netaddr_get_network_order_ip((na as *const NetAddr).cast()) };
                let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
                out.push(ipv4);
            }
            happy_eyeballs::DnsResultInner::A(Ok(out))
        };

        let input = happy_eyeballs::Input::DnsResult(happy_eyeballs::DnsResult {
            target_name: name,
            inner,
        });
        self.inner.process_input(input);

        NS_OK
    }

    fn process_dns_response_aaaa(
        &mut self,
        hostname: *const nsACString,
        addrs: *const NetAddr,
        addrs_len: u32,
    ) -> nsresult {
        if hostname.is_null() {
            return NS_ERROR_UNEXPECTED;
        }
        let host = unsafe { (&*hostname).to_utf8().to_string() };
        let name = happy_eyeballs::TargetName::from(host.as_str());

        let inner = if addrs_len == 0 {
            happy_eyeballs::DnsResultInner::Aaaa(Ok(Vec::new()))
        } else {
            if addrs.is_null() {
                return NS_ERROR_UNEXPECTED;
            }
            let slice = unsafe { std::slice::from_raw_parts(addrs, addrs_len as usize) };
            let mut out = Vec::with_capacity(slice.len());
            for na in slice.iter() {
                let family = i32::from(unsafe { moz_netaddr_get_family((na as *const NetAddr).cast()) });
                debug_assert_eq!(family, AF_INET6, "Expected IPv6 address in AAAA record response");
                if family != AF_INET6 {
                    return NS_ERROR_UNEXPECTED;
                }
                let p = unsafe { moz_netaddr_get_ipv6((na as *const NetAddr).cast()) };
                let octs: [u8; 16] =
                    unsafe { std::slice::from_raw_parts(p, 16).try_into().unwrap() };
                let ipv6 = Ipv6Addr::from(octs);
                out.push(ipv6);
            }
            happy_eyeballs::DnsResultInner::Aaaa(Ok(out))
        };

        let input = happy_eyeballs::Input::DnsResult(happy_eyeballs::DnsResult {
            target_name: name,
            inner,
        });
        self.inner.process_input(input);

        NS_OK
    }

    fn process_dns_response_https(
        &mut self,
        hostname: *const nsACString,
        priority: u16,
        target_name: *const nsACString,
        alpn_protocols: *const Protocol,
        alpn_protocols_len: u32,
        ech_config: *const u8,
        ech_config_len: u32,
        ipv4_hints: *const NetAddr,
        ipv4_hints_len: u32,
        ipv6_hints: *const NetAddr,
        ipv6_hints_len: u32,
    ) -> nsresult {
        if hostname.is_null() {
            return NS_ERROR_UNEXPECTED;
        }
        let host = unsafe { (&*hostname).to_utf8().to_string() };
        let name = happy_eyeballs::TargetName::from(host.as_str());

        let target = if !target_name.is_null() {
            let t = unsafe { (&*target_name).to_utf8().to_string() };
            happy_eyeballs::TargetName::from(t.as_str())
        } else {
            name.clone()
        };

        let mut alpn_set = std::collections::HashSet::new();
        if !alpn_protocols.is_null() && alpn_protocols_len > 0 {
            let alpn_slice =
                unsafe { std::slice::from_raw_parts(alpn_protocols, alpn_protocols_len as usize) };
            for protocol in alpn_slice {
                alpn_set.insert((*protocol).into());
            }
        }

        let ech = if !ech_config.is_null() && ech_config_len > 0 {
            Some(
                unsafe { std::slice::from_raw_parts(ech_config, ech_config_len as usize) }
                    .to_vec(),
            )
        } else {
            None
        };

        let mut ipv4_vec = Vec::new();
        if !ipv4_hints.is_null() && ipv4_hints_len > 0 {
            let hints_slice =
                unsafe { std::slice::from_raw_parts(ipv4_hints, ipv4_hints_len as usize) };
            for na in hints_slice {
                let family = i32::from(unsafe { moz_netaddr_get_family((na as *const NetAddr).cast()) });
                debug_assert_eq!(family, AF_INET, "Expected IPv4 address in IPv4 hints");
                if family != AF_INET {
                    return NS_ERROR_UNEXPECTED;
                }
                let ip_be =
                    unsafe { moz_netaddr_get_network_order_ip((na as *const NetAddr).cast()) };
                let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
                ipv4_vec.push(ipv4);
            }
        }

        let mut ipv6_vec = Vec::new();
        if !ipv6_hints.is_null() && ipv6_hints_len > 0 {
            let hints_slice =
                unsafe { std::slice::from_raw_parts(ipv6_hints, ipv6_hints_len as usize) };
            for na in hints_slice {
                let family = i32::from(unsafe { moz_netaddr_get_family((na as *const NetAddr).cast()) });
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
        }

        let service_info = happy_eyeballs::ServiceInfo {
            priority,
            target_name: target,
            alpn_protocols: alpn_set,
            ech_config: ech,
            ipv4_hints: ipv4_vec,
            ipv6_hints: ipv6_vec,
        };

        // TODO: Instead of providing them individually, a better approach would
        // be providing all svcb records at once. Difficult to design a clean
        // FFI for it. Ideas?
        let inner = happy_eyeballs::DnsResultInner::Https(Ok(vec![service_info]));

        let input = happy_eyeballs::Input::DnsResult(happy_eyeballs::DnsResult {
            target_name: name,
            inner,
        });
        self.inner.process_input(input);

        NS_OK
    }

    fn process_connection_result(
        &mut self,
        addr: *const NetAddr,
        status: nsresult,
    ) -> nsresult {
        if addr.is_null() {
            return NS_ERROR_UNEXPECTED;
        }
        let netaddr = unsafe { &*addr };
        let port = u16::from_be(unsafe { moz_netaddr_get_network_order_port(netaddr) });

        let family = i32::from(unsafe { moz_netaddr_get_family(netaddr) });
        let address = if family == AF_INET {
            let ip_be = unsafe { moz_netaddr_get_network_order_ip(netaddr) };
            let ipv4 = Ipv4Addr::from(u32::from_be(ip_be));
            SocketAddr::from((ipv4, port))
        } else if family == AF_INET6 {
            let ipv6_ptr = unsafe { moz_netaddr_get_ipv6(netaddr) };
            let octs: [u8; 16] =
                unsafe { std::slice::from_raw_parts(ipv6_ptr, 16).try_into().unwrap() };
            let ipv6 = Ipv6Addr::from(octs);
            SocketAddr::from((ipv6, port))
        } else {
            return NS_ERROR_UNEXPECTED;
        };

        let result = if status == NS_OK {
            Ok(())
        } else {
            Err(format!("connection failed: 0x{:08x}", status.0))
        };

        let input = happy_eyeballs::Input::ConnectionResult { address, result };
        self.inner.process_input(input);

        NS_OK
    }

    fn process_output(&mut self, ret_event: &mut Output, data: &mut ThinVec<u8>) -> nsresult {
        let out = self.inner.process_output(std::time::Instant::now());
        data.clear();
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
                let addr_str = endpoint.address.ip().to_string();
                data.extend_from_slice(addr_str.as_bytes());
                *ret_event = Output::AttemptConnection {
                    protocol: endpoint.protocol.into(),
                    port: endpoint.address.port(),
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

#[no_mangle]
pub extern "C" fn happy_eyeballs_new(
    result: &mut *const HappyEyeballs,
    origin: *const nsACString,
    port: u16,
    alt_svc: *const AltSvc,
    alt_svc_len: u32,
) -> nsresult {
    *result = ptr::null_mut();

    if origin.is_null() {
        return NS_ERROR_INVALID_ARG;
    }

    let origin_str = unsafe { (&*origin).to_utf8().to_string() };

    let alt_svc_vec = if !alt_svc.is_null() && alt_svc_len > 0 {
        let slice = unsafe { std::slice::from_raw_parts(alt_svc, alt_svc_len as usize) };
        slice
            .iter()
            .map(|a| happy_eyeballs::AltSvc {
                host: None,
                port: None,
                protocol: a.protocol.into(),
            })
            .collect()
    } else {
        Vec::new()
    };

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
pub enum ProtocolCombination {
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
pub enum Output {
    SendDnsQuery { record_type: DnsRecordType },
    Timer { duration_ms: u64 },
    AttemptConnection { protocol: ProtocolCombination, port: u16 },
    CancelConnection { port: u16 },
    Succeeded,
    Failed,
    None,
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_a(
    he: &mut HappyEyeballs,
    hostname: *const nsACString,
    addrs: *const NetAddr,
    addrs_len: u32,
) -> nsresult {
    he.process_dns_response_a(hostname, addrs, addrs_len)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_aaaa(
    he: &mut HappyEyeballs,
    hostname: *const nsACString,
    addrs: *const NetAddr,
    addrs_len: u32,
) -> nsresult {
    he.process_dns_response_aaaa(hostname, addrs, addrs_len)
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_dns_response_https(
    he: &mut HappyEyeballs,
    hostname: *const nsACString,
    priority: u16,
    target_name: *const nsACString,
    alpn_protocols: *const Protocol,
    alpn_protocols_len: u32,
    ech_config: *const u8,
    ech_config_len: u32,
    ipv4_hints: *const NetAddr,
    ipv4_hints_len: u32,
    ipv6_hints: *const NetAddr,
    ipv6_hints_len: u32,
) -> nsresult {
    he.process_dns_response_https(
        hostname,
        priority,
        target_name,
        alpn_protocols,
        alpn_protocols_len,
        ech_config,
        ech_config_len,
        ipv4_hints,
        ipv4_hints_len,
        ipv6_hints,
        ipv6_hints_len,
    )
}

#[no_mangle]
pub extern "C" fn happy_eyeballs_process_connection_result(
    he: &mut HappyEyeballs,
    addr: *const NetAddr,
    status: nsresult,
) -> nsresult {
    he.process_connection_result(addr, status)
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
