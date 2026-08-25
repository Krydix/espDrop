use std::env;
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const ESPRESSIF_VID: u16 = 0x303a;
const SPOOL_CAPACITY: u64 = 0x9d0000 - 4096;
const ODC_HEADER_BYTES: u64 = 76;
const ODC_BLOCK_BYTES: u64 = 10_240;
const DVZIP_BLOCK_BYTES: u64 = 131_072;
const STREAM_CHUNK_BYTES: usize = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct PreparedPlan {
    file_bytes: u64,
    archive_bytes: u64,
    dvzip_blocks: u64,
    payload_bytes: u64,
}

#[derive(Debug, PartialEq, Eq)]
struct StreamCompletion {
    http_status: u16,
    request_bytes: u64,
    payload_bytes: u64,
}

impl PreparedPlan {
    fn new(file_bytes: u64, file_name: &str) -> Result<Self, String> {
        let path_bytes = 2_u64
            .checked_add(file_name.len() as u64)
            .and_then(|bytes| bytes.checked_add(1))
            .ok_or("archive path length overflow")?;
        let trailer_bytes = 11_u64;
        let unpadded = ODC_HEADER_BYTES
            .checked_mul(2)
            .and_then(|bytes| bytes.checked_add(path_bytes))
            .and_then(|bytes| bytes.checked_add(trailer_bytes))
            .and_then(|bytes| bytes.checked_add(file_bytes))
            .ok_or("archive size overflow")?;
        let archive_bytes = unpadded
            .checked_add(ODC_BLOCK_BYTES - 1)
            .map(|bytes| bytes / ODC_BLOCK_BYTES * ODC_BLOCK_BYTES)
            .ok_or("archive padding overflow")?;
        let dvzip_blocks = archive_bytes.div_ceil(DVZIP_BLOCK_BYTES);
        let payload_bytes = archive_bytes
            .checked_add(
                dvzip_blocks
                    .checked_mul(4)
                    .ok_or("dvzip header size overflow")?,
            )
            .ok_or("dvzip payload size overflow")?;
        Ok(Self {
            file_bytes,
            archive_bytes,
            dvzip_blocks,
            payload_bytes,
        })
    }
}

fn odc_header(
    inode: u32,
    mode: u32,
    mtime: u32,
    name_bytes: u64,
    file_bytes: u64,
) -> io::Result<[u8; 76]> {
    let value = format!(
        "070707{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:011o}{:06o}{:011o}",
        0, inode, mode, 0, 0, 1, 0, mtime, name_bytes, file_bytes
    );
    let bytes = value.as_bytes();
    if bytes.len() != 76 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "file metadata does not fit an odc cpio header",
        ));
    }
    let mut header = [0_u8; 76];
    header.copy_from_slice(bytes);
    Ok(header)
}

struct ArchiveReader<R> {
    source: R,
    segments: Vec<Box<[u8]>>,
    segment: usize,
    segment_offset: usize,
    file_remaining: u64,
    padding_remaining: u64,
}

impl<R: Read> ArchiveReader<R> {
    fn new(source: R, file_name: &str, plan: PreparedPlan) -> io::Result<Self> {
        let archive_path = format!("./{file_name}\0").into_bytes();
        let trailer = b"TRAILER!!!\0".to_vec();
        let file_header = odc_header(1, 0o100644, 0, archive_path.len() as u64, plan.file_bytes)?;
        let trailer_header = odc_header(0, 0, 0, trailer.len() as u64, 0)?;
        let unpadded = file_header.len() as u64
            + archive_path.len() as u64
            + plan.file_bytes
            + trailer_header.len() as u64
            + trailer.len() as u64;
        Ok(Self {
            source,
            segments: vec![
                file_header.to_vec().into_boxed_slice(),
                archive_path.into_boxed_slice(),
                trailer_header.to_vec().into_boxed_slice(),
                trailer.into_boxed_slice(),
            ],
            segment: 0,
            segment_offset: 0,
            file_remaining: plan.file_bytes,
            padding_remaining: plan.archive_bytes - unpadded,
        })
    }

    fn read_segment(&mut self, output: &mut [u8], segment: usize) -> usize {
        let value = &self.segments[segment];
        let available = value.len() - self.segment_offset;
        let count = output.len().min(available);
        output[..count].copy_from_slice(&value[self.segment_offset..self.segment_offset + count]);
        self.segment_offset += count;
        if self.segment_offset == value.len() {
            self.segment += 1;
            self.segment_offset = 0;
        }
        count
    }
}

impl<R: Read> Read for ArchiveReader<R> {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        if output.is_empty() {
            return Ok(0);
        }
        let mut written = 0;
        while written < output.len() {
            if self.segment == 0 || self.segment == 1 {
                written += self.read_segment(&mut output[written..], self.segment);
            } else if self.segment == 2 && self.file_remaining > 0 {
                let request = (output.len() - written).min(self.file_remaining as usize);
                let count = self.source.read(&mut output[written..written + request])?;
                if count == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "source file changed or was truncated while packaging",
                    ));
                }
                written += count;
                self.file_remaining -= count as u64;
            } else if self.segment == 2 {
                self.segment = 3;
            } else if self.segment == 3 || self.segment == 4 {
                written += self.read_segment(&mut output[written..], self.segment - 1);
            } else if self.padding_remaining > 0 {
                let count = (output.len() - written).min(self.padding_remaining as usize);
                output[written..written + count].fill(0);
                written += count;
                self.padding_remaining -= count as u64;
            } else {
                break;
            }
        }
        Ok(written)
    }
}

struct StoredDvzipReader<R> {
    archive: ArchiveReader<R>,
    archive_remaining: u64,
    block_remaining: u64,
    header: [u8; 4],
    header_offset: usize,
}

impl<R: Read> StoredDvzipReader<R> {
    fn new(source: R, file_name: &str, plan: PreparedPlan) -> io::Result<Self> {
        Ok(Self {
            archive: ArchiveReader::new(source, file_name, plan)?,
            archive_remaining: plan.archive_bytes,
            block_remaining: 0,
            header: [0; 4],
            header_offset: 4,
        })
    }
}

impl<R: Read> Read for StoredDvzipReader<R> {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        if output.is_empty() {
            return Ok(0);
        }
        let mut written = 0;
        while written < output.len() && self.archive_remaining > 0 {
            if self.block_remaining == 0 && self.header_offset == 4 {
                self.block_remaining = self.archive_remaining.min(DVZIP_BLOCK_BYTES);
                let encoded = 0x8000_0000_u32 | self.block_remaining as u32;
                self.header = encoded.to_be_bytes();
                self.header_offset = 0;
            }
            if self.header_offset < 4 {
                let count = (output.len() - written).min(4 - self.header_offset);
                output[written..written + count]
                    .copy_from_slice(&self.header[self.header_offset..self.header_offset + count]);
                self.header_offset += count;
                written += count;
                continue;
            }
            let request = (output.len() - written).min(self.block_remaining as usize);
            let count = self.archive.read(&mut output[written..written + request])?;
            if count == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "generated cpio archive ended before its declared size",
                ));
            }
            written += count;
            self.block_remaining -= count as u64;
            self.archive_remaining -= count as u64;
        }
        Ok(written)
    }
}

fn normalized_serial(value: &str) -> String {
    value
        .chars()
        .filter(|character| *character != ':' && *character != '-')
        .flat_map(char::to_uppercase)
        .collect()
}

fn ports() -> Result<(), Box<dyn std::error::Error>> {
    let mut found = false;
    for port in serialport::available_ports()? {
        let serialport::SerialPortType::UsbPort(usb) = port.port_type else {
            continue;
        };
        if usb.vid != ESPRESSIF_VID {
            continue;
        }
        found = true;
        println!(
            "{}: Espressif USB {:04x}:{:04x}; serial={}",
            port.port_name,
            usb.vid,
            usb.pid,
            usb.serial_number.as_deref().unwrap_or("unknown")
        );
    }
    if !found {
        println!("No Espressif USB serial ports found.");
    }
    Ok(())
}

fn verify_target(port_name: &str, expected_serial: &str) -> Result<String, String> {
    let port = serialport::available_ports()
        .map_err(|error| error.to_string())?
        .into_iter()
        .find(|port| port.port_name == port_name)
        .ok_or_else(|| format!("target {port_name} is not an attached serial device"))?;
    let serialport::SerialPortType::UsbPort(usb) = port.port_type else {
        return Err(format!(
            "refusing {port_name}: it is not a USB serial device"
        ));
    };
    let actual = usb.serial_number.as_deref().unwrap_or("");
    if usb.vid != ESPRESSIF_VID || normalized_serial(actual) != normalized_serial(expected_serial) {
        return Err(format!(
            "refusing {port_name}: expected Espressif serial {expected_serial}, found VID={:#06x} serial={}",
            usb.vid,
            if actual.is_empty() { "unknown" } else { actual }
        ));
    }
    Ok(actual.to_owned())
}

fn inferred_type(path: &Path) -> Option<&'static str> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    match extension.as_str() {
        "jpg" | "jpeg" => Some("public.jpeg"),
        "png" => Some("public.png"),
        "heic" | "heif" => Some("public.heic"),
        "pdf" => Some("com.adobe.pdf"),
        "txt" => Some("public.plain-text"),
        "wav" => Some("com.microsoft.waveform-audio"),
        _ => None,
    }
}

fn file_crc32(path: &Path) -> io::Result<u32> {
    let mut source = File::open(path)?;
    let mut hasher = crc32fast::Hasher::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = source.read(&mut buffer)?;
        if count == 0 {
            return Ok(hasher.finalize());
        }
        hasher.update(&buffer[..count]);
    }
}

fn reader_crc32(mut source: impl Read) -> io::Result<(u64, u32)> {
    let mut hasher = crc32fast::Hasher::new();
    let mut total = 0_u64;
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = source.read(&mut buffer)?;
        if count == 0 {
            return Ok((total, hasher.finalize()));
        }
        hasher.update(&buffer[..count]);
        total += count as u64;
    }
}

fn validate_send_metadata(
    path: &Path,
    explicit_type: Option<&str>,
) -> Result<(u64, String, String), Box<dyn std::error::Error>> {
    let metadata = path.metadata()?;
    if !metadata.is_file() {
        return Err(format!("not a regular file: {}", path.display()).into());
    }
    if metadata.len() == 0 || metadata.len() > 64 * 1024 * 1024 {
        return Err(format!(
            "file is {} bytes; this lab firmware accepts 1..=67108864 bytes",
            metadata.len()
        )
        .into());
    }
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("the file name must be valid UTF-8")?;
    if file_name.is_empty()
        || file_name.len() > 127
        || !file_name
            .bytes()
            .all(|byte| (0x20..=0x7e).contains(&byte) && byte != b'/' && byte != b'\\')
        || matches!(file_name, "." | "..")
    {
        return Err(
            "the AirDrop file name must be 1-127 printable ASCII bytes without slashes".into(),
        );
    }
    let file_type = explicit_type
        .or_else(|| inferred_type(path))
        .ok_or("unknown file extension; pass --type with an Apple UTI")?;
    if file_type.is_empty()
        || file_type.len() > 63
        || !file_type
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'.' || byte == b'-')
    {
        return Err(
            "file type must be a 1-63 byte UTI using letters, digits, dots, or hyphens".into(),
        );
    }
    Ok((metadata.len(), file_name.to_owned(), file_type.to_owned()))
}

fn wait_for_marker(
    port: &mut dyn serialport::SerialPort,
    wanted: &[u8],
    timeout: Duration,
) -> Result<Vec<u8>, String> {
    let deadline = Instant::now() + timeout;
    const STREAM_GO: &[u8] = b"ESPDROP-STREAM-GO";
    const TRACE_PATTERNS: [&str; 15] = [
        "AWDL-AIRDROP",
        "AWDL-TARGET",
        "AUTO-TARGET",
        "TX-LAB-TARGET",
        "TX-LAB-PEER-READY",
        "TX-LAB-ELECTION",
        "TX-LAB-SCHEDULE",
        "TX-LAB-REACTION",
        "TX-LAB-NA",
        "TX-LAB-ECHO-REPLY",
        "TX-LAB-SUMMARY",
        "AIRDROP-TLS",
        "AIRDROP-ASK",
        "ESPDROP-STREAM",
        "relay_stream",
    ];
    let trace_discovery = wanted == STREAM_GO;
    let mut received = Vec::new();
    let mut chunk = [0_u8; 1024];
    let mut trace_line = Vec::new();
    while Instant::now() < deadline {
        match port.read(&mut chunk) {
            Ok(count) => {
                received.extend_from_slice(&chunk[..count]);
                if trace_discovery {
                    for byte in &chunk[..count] {
                        if *byte == b'\n' {
                            let line = String::from_utf8_lossy(&trace_line);
                            if TRACE_PATTERNS.iter().any(|pattern| line.contains(pattern)) {
                                eprintln!("esp> {}", line.trim_end_matches('\r'));
                            }
                            trace_line.clear();
                        } else if trace_line.len() < 2048 {
                            trace_line.push(*byte);
                        } else {
                            trace_line.clear();
                        }
                    }
                }
            }
            Err(error) if error.kind() == io::ErrorKind::TimedOut => {}
            Err(error) => return Err(format!("serial read failed: {error}")),
        }
        if received
            .windows(wanted.len())
            .any(|window| window == wanted)
        {
            return Ok(received);
        }
        for failure in [
            b"ESPDROP-RELAY-INVALID".as_slice(),
            b"ESPDROP-RELAY-BUSY".as_slice(),
            b"ESPDROP-RELAY-CRC-ERROR".as_slice(),
            b"ESPDROP-RELAY-FAILED".as_slice(),
            b"ESPDROP-RELAY-TIMEOUT".as_slice(),
            b"ESPDROP-STREAM-INVALID".as_slice(),
            b"ESPDROP-STREAM-BUSY".as_slice(),
            b"ESPDROP-STREAM-FAILED".as_slice(),
            b"ESPDROP-STREAM-DATA-INVALID".as_slice(),
            b"ESPDROP-STREAM-CHUNK-ERROR".as_slice(),
            b"ESPDROP-STREAM-SOURCE-TIMEOUT".as_slice(),
            b"ESPDROP-STREAM-TIMEOUT".as_slice(),
        ] {
            if received
                .windows(failure.len())
                .any(|window| window == failure)
            {
                return Err(String::from_utf8_lossy(&received).trim().to_owned());
            }
        }
        if received.len() > 64 * 1024 {
            let keep = received.len() - 32 * 1024;
            received.drain(..keep);
        }
    }
    Err(format!(
        "timed out waiting for {}",
        String::from_utf8_lossy(wanted)
    ))
}

fn parse_stream_result(line: &str) -> Result<Option<StreamCompletion>, String> {
    if !line.starts_with("ESPDROP-STREAM-RESULT ") {
        return Ok(None);
    }
    let field = |name: &str| {
        line.split_whitespace()
            .find_map(|value| value.strip_prefix(name))
    };
    match field("state=") {
        Some("none" | "pending") => Ok(None),
        Some("failed") => Err(line.to_owned()),
        Some("success") => {
            if field("stage=") != Some("upload") {
                return Err(format!("invalid successful stream result: {line}"));
            }
            let http_status = field("status=")
                .ok_or_else(|| format!("stream result has no HTTP status: {line}"))?
                .parse::<u16>()
                .map_err(|_| format!("stream result has an invalid HTTP status: {line}"))?;
            if http_status != 200 {
                return Err(format!("successful stream result is not HTTP 200: {line}"));
            }
            let request_bytes = field("request_bytes=")
                .ok_or_else(|| format!("stream result has no request size: {line}"))?
                .parse::<u64>()
                .map_err(|_| format!("stream result has an invalid request size: {line}"))?;
            let payload_bytes = field("payload_bytes=")
                .ok_or_else(|| format!("stream result has no payload size: {line}"))?
                .parse::<u64>()
                .map_err(|_| format!("stream result has an invalid payload size: {line}"))?;
            Ok(Some(StreamCompletion {
                http_status,
                request_bytes,
                payload_bytes,
            }))
        }
        _ => Err(format!("invalid stream result: {line}")),
    }
}

fn wait_for_stream_completion(
    port: &mut dyn serialport::SerialPort,
    timeout: Duration,
) -> Result<StreamCompletion, String> {
    let deadline = Instant::now() + timeout;
    let mut line = Vec::new();
    let mut chunk = [0_u8; 1024];
    while Instant::now() < deadline {
        writeln!(port, "ESPDROP STREAM RESULT")
            .map_err(|error| format!("serial write failed: {error}"))?;
        port.flush()
            .map_err(|error| format!("serial flush failed: {error}"))?;
        let poll_deadline = (Instant::now() + Duration::from_secs(2)).min(deadline);
        while Instant::now() < poll_deadline {
            match port.read(&mut chunk) {
                Ok(count) => {
                    for byte in &chunk[..count] {
                        if *byte == b'\n' {
                            let value = String::from_utf8_lossy(&line)
                                .trim_end_matches('\r')
                                .to_owned();
                            line.clear();
                            if value.starts_with("ESPDROP-STREAM-RESULT ") {
                                match parse_stream_result(&value)? {
                                    Some(result) => return Ok(result),
                                    None => break,
                                }
                            }
                        } else if line.len() < 4096 {
                            line.push(*byte);
                        } else {
                            line.clear();
                        }
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::TimedOut => {}
                Err(error) => return Err(format!("serial read failed: {error}")),
            }
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Err("timed out waiting for the final AirDrop Upload result".to_owned())
}

fn open_control_port(
    port_name: &str,
) -> Result<Box<dyn serialport::SerialPort>, Box<dyn std::error::Error>> {
    let port = serialport::new(port_name, 115_200)
        .timeout(Duration::from_millis(200))
        .preserve_dtr_on_open()
        .open()?;
    /* Do not change DTR/RTS after opening native USB Serial/JTAG. Clearing
     * both lines is interpreted as USB_UART_CHIP_RESET by the ESP32-S3. */
    port.clear(serialport::ClearBuffer::Input)?;
    Ok(port)
}

fn validate_peer_target(value: &str) -> Result<String, String> {
    if value.eq_ignore_ascii_case("auto")
        || value.eq_ignore_ascii_case("none")
        || value.eq_ignore_ascii_case("only")
    {
        return Ok(value.to_ascii_uppercase());
    }
    let compact = normalized_serial(value);
    if compact.len() != 12 || !compact.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err("target must be AUTO, NONE, ONLY, or a six-byte MAC address".into());
    }
    Ok(compact
        .as_bytes()
        .chunks(2)
        .map(|pair| String::from_utf8_lossy(pair).to_ascii_lowercase())
        .collect::<Vec<_>>()
        .join(":"))
}

fn query_peer_lines(
    port: &mut dyn serialport::SerialPort,
) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    writeln!(port, "ESPDROP PEERS")?;
    port.flush()?;
    let response = wait_for_marker(port, b"ESPDROP-PEERS-END", Duration::from_secs(5))?;
    Ok(String::from_utf8_lossy(&response)
        .lines()
        .filter(|line| line.starts_with("ESPDROP-PEER"))
        .map(str::to_owned)
        .collect())
}

fn peer_field<'a>(line: &'a str, name: &str) -> Option<&'a str> {
    line.split_whitespace()
        .find_map(|field| field.strip_prefix(name))
}

fn peer_mac_from_line(line: &str, complete: bool) -> Option<&str> {
    if complete {
        if peer_field(line, "endpoint=") != Some("1")
            || peer_field(line, "airdrop_age_ms=")?.parse::<u64>().ok()? > 5_000
        {
            return None;
        }
    } else if peer_field(line, "valid=") != Some("1")
        || peer_field(line, "age_ms=")?.parse::<u64>().ok()? > 5_000
    {
        return None;
    }
    peer_field(line, "mac=")
}

fn wait_for_only_peer(
    port: &mut dyn serialport::SerialPort,
) -> Result<String, Box<dyn std::error::Error>> {
    let deadline = Instant::now() + Duration::from_secs(90);
    let raw_peer_settle = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        let lines = query_peer_lines(port)?;
        let mut peers: Vec<&str> = lines
            .iter()
            .filter_map(|line| peer_mac_from_line(line, true))
            .collect();
        peers.sort_unstable();
        peers.dedup();
        match peers.as_slice() {
            [peer] => {
                println!("host selected the only complete AirDrop receiver: {peer}");
                return Ok((*peer).to_owned());
            }
            [] => {
                let mut raw_peers: Vec<&str> = lines
                    .iter()
                    .filter_map(|line| peer_mac_from_line(line, false))
                    .collect();
                raw_peers.sort_unstable();
                raw_peers.dedup();
                if Instant::now() >= raw_peer_settle {
                    match raw_peers.as_slice() {
                        [peer] => {
                            println!("host selected the only live raw AWDL peer: {peer}");
                            return Ok((*peer).to_owned());
                        }
                        [_, _, ..] => {
                            for line in lines {
                                println!("{line}");
                            }
                            return Err(
                                "multiple live AWDL peers are visible; choose one MAC with --target"
                                    .into(),
                            );
                        }
                        [] => {}
                    }
                }
                std::thread::sleep(Duration::from_millis(750));
            }
            _ => {
                for line in lines {
                    println!("{line}");
                }
                return Err(
                    "multiple complete receivers are visible; choose one MAC with --target".into(),
                );
            }
        }
    }
    Err("timed out waiting for one complete AirDrop receiver".into())
}

fn set_target(
    port: &mut dyn serialport::SerialPort,
    target: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let target = validate_peer_target(target)?;
    writeln!(port, "ESPDROP TARGET {target}")?;
    port.flush()?;
    wait_for_marker(port, b"ESPDROP-TARGET mode=", Duration::from_secs(5))?;
    println!("ESP target selection set to {target}");
    Ok(())
}

fn cancel_stream(port: &mut dyn serialport::SerialPort) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(port, "ESPDROP STREAM CANCEL")?;
    port.flush()?;
    let response = wait_for_marker(port, b"ESPDROP-STREAM-CANCEL", Duration::from_secs(5))?;
    if response
        .windows(b"ESPDROP-STREAM-CANCELLED".len())
        .any(|window| window == b"ESPDROP-STREAM-CANCELLED")
    {
        return Ok(());
    }
    Err("the prior stream is already transferring; wait for it to finish".into())
}

fn restart_and_wait(
    port_name: &str,
    expected_serial: &str,
) -> Result<Box<dyn serialport::SerialPort>, Box<dyn std::error::Error>> {
    verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    writeln!(port, "ESPDROP RESTART")?;
    port.flush()?;
    wait_for_marker(port.as_mut(), b"ESPDROP-RESTARTING", Duration::from_secs(3))?;
    drop(port);

    let deadline = Instant::now() + Duration::from_secs(20);
    let mut last_error = String::from("device has not reappeared");
    std::thread::sleep(Duration::from_millis(1200));
    while Instant::now() < deadline {
        if verify_target(port_name, expected_serial).is_ok() {
            match open_control_port(port_name) {
                Ok(mut candidate) => {
                    if writeln!(candidate, "ESPDROP PING").is_ok() && candidate.flush().is_ok() {
                        if let Ok(response) = wait_for_marker(
                            candidate.as_mut(),
                            b"ESPDROP-PONG",
                            Duration::from_millis(800),
                        ) {
                            if response
                                .windows(b"ready=1".len())
                                .any(|window| window == b"ready=1")
                            {
                                println!("espDrop restarted and application is ready");
                                return Ok(candidate);
                            }
                            last_error =
                                "firmware control is up; radio initialization is pending".into();
                        }
                    }
                }
                Err(error) => last_error = error.to_string(),
            }
        }
        std::thread::sleep(Duration::from_millis(750));
    }
    Err(format!("timed out waiting for espDrop restart: {last_error}").into())
}

fn list_peers(port_name: &str, expected_serial: &str) -> Result<(), Box<dyn std::error::Error>> {
    let actual_serial = verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    let lines = query_peer_lines(port.as_mut())?;
    println!("verified {port_name}; Espressif serial/MAC={actual_serial}");
    for line in lines {
        println!("{line}");
    }
    Ok(())
}

fn ping(port_name: &str, expected_serial: &str) -> Result<(), Box<dyn std::error::Error>> {
    let actual_serial = verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    writeln!(port, "ESPDROP PING")?;
    port.flush()?;
    let mut response = wait_for_marker(port.as_mut(), b"ESPDROP-PONG", Duration::from_secs(5))?;
    let marker_offset = response
        .windows(b"ESPDROP-PONG".len())
        .position(|window| window == b"ESPDROP-PONG")
        .ok_or("ESP returned no PONG marker")?;
    let deadline = Instant::now() + Duration::from_secs(1);
    let mut chunk = [0_u8; 128];
    while !response[marker_offset..].contains(&b'\n') && Instant::now() < deadline {
        match port.read(&mut chunk) {
            Ok(count) => response.extend_from_slice(&chunk[..count]),
            Err(error) if error.kind() == io::ErrorKind::TimedOut => {}
            Err(error) => return Err(error.into()),
        }
    }
    let pong = String::from_utf8_lossy(&response);
    let line = pong
        .lines()
        .find_map(|line| line.find("ESPDROP-PONG").map(|offset| &line[offset..]))
        .ok_or("ESP returned no PONG record")?;
    println!("verified {port_name}; Espressif serial/MAC={actual_serial}");
    println!("{line}");
    Ok(())
}

fn stats(port_name: &str, expected_serial: &str) -> Result<(), Box<dyn std::error::Error>> {
    let actual_serial = verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    writeln!(port, "ESPDROP STATS")?;
    port.flush()?;
    let mut response = wait_for_marker(port.as_mut(), b"ESPDROP-STATS", Duration::from_secs(5))?;
    let marker_offset = response
        .windows(b"ESPDROP-STATS".len())
        .position(|window| window == b"ESPDROP-STATS")
        .ok_or("ESP returned no transport stats marker")?;
    let deadline = Instant::now() + Duration::from_secs(1);
    let mut chunk = [0_u8; 256];
    while !response[marker_offset..].contains(&b'\n') && Instant::now() < deadline {
        match port.read(&mut chunk) {
            Ok(count) => response.extend_from_slice(&chunk[..count]),
            Err(error) if error.kind() == io::ErrorKind::TimedOut => {}
            Err(error) => return Err(error.into()),
        }
    }
    let text = String::from_utf8_lossy(&response);
    let line = text
        .lines()
        .find_map(|line| line.find("ESPDROP-STATS").map(|offset| &line[offset..]))
        .ok_or("ESP returned no transport stats record")?;
    println!("verified {port_name}; Espressif serial/MAC={actual_serial}");
    println!("{line}");
    Ok(())
}

fn wake(port_name: &str, expected_serial: &str) -> Result<(), Box<dyn std::error::Error>> {
    let actual_serial = verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    writeln!(port, "ESPDROP WAKE")?;
    port.flush()?;
    wait_for_marker(port.as_mut(), b"ESPDROP-WAKE-ARMED", Duration::from_secs(5))?;
    println!("verified {port_name}; Espressif serial/MAC={actual_serial}");
    println!("AirDrop BLE wake window armed");
    Ok(())
}

fn control_target(
    port_name: &str,
    expected_serial: &str,
    target: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let actual_serial = verify_target(port_name, expected_serial)?;
    let mut port = open_control_port(port_name)?;
    let selected_target = if target.eq_ignore_ascii_case("only") {
        wait_for_only_peer(port.as_mut())?
    } else {
        target.to_owned()
    };
    set_target(port.as_mut(), &selected_target)?;
    println!("verified {port_name}; Espressif serial/MAC={actual_serial}");
    Ok(())
}

fn restart(port_name: &str, expected_serial: &str) -> Result<(), Box<dyn std::error::Error>> {
    drop(restart_and_wait(port_name, expected_serial)?);
    Ok(())
}

fn send(
    port_name: &str,
    expected_serial: &str,
    path: &Path,
    explicit_type: Option<&str>,
    target: &str,
    restart_first: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let (file_bytes, file_name, file_type) = validate_send_metadata(path, explicit_type)?;
    let plan = PreparedPlan::new(file_bytes, &file_name)?;
    let file_crc32 = file_crc32(path)?;
    let payload_for_crc = StoredDvzipReader::new(File::open(path)?, &file_name, plan)?;
    let (generated_bytes, payload_crc32) = reader_crc32(payload_for_crc)?;
    if generated_bytes != plan.payload_bytes {
        return Err(format!(
            "internal packaging mismatch: planned {} bytes, generated {generated_bytes}",
            plan.payload_bytes
        )
        .into());
    }

    let actual_serial = verify_target(port_name, expected_serial)?;
    println!(
        "verified {port_name}; Espressif serial/MAC={actual_serial}\nprepared {}: file={} bytes crc32={file_crc32:08x}, cpio={} bytes, dvzip={} bytes crc32={payload_crc32:08x}, blocks={}, type={file_type}",
        path.display(),
        plan.file_bytes,
        plan.archive_bytes,
        plan.payload_bytes,
        plan.dvzip_blocks,
    );

    let mut port = if restart_first {
        restart_and_wait(port_name, expected_serial)?
    } else {
        open_control_port(port_name)?
    };
    /* A host process can disappear while waiting for /Ask. Clear that stale
     * session before arming its replacement; this makes retries reset-free. */
    cancel_stream(port.as_mut())?;
    let selected_target = if target.eq_ignore_ascii_case("only") {
        wait_for_only_peer(port.as_mut())?
    } else {
        target.to_owned()
    };
    set_target(port.as_mut(), &selected_target)?;
    let name_hex: String = file_name
        .as_bytes()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    writeln!(
        port,
        "ESPDROP STREAM BEGIN {} {file_crc32:08x} {} {payload_crc32:08x} {} {} {name_hex} {file_type}",
        plan.file_bytes, plan.payload_bytes, plan.archive_bytes, plan.dvzip_blocks
    )?;
    port.flush()?;
    wait_for_marker(
        port.as_mut(),
        b"ESPDROP-STREAM-ARMED",
        Duration::from_secs(30),
    )?;
    println!("stream armed; waiting for the receiver to accept the AirDrop prompt…");
    wait_for_marker(
        port.as_mut(),
        b"ESPDROP-STREAM-GO",
        Duration::from_secs(300),
    )?;
    println!("receiver accepted; streaming prepared payload with per-chunk acknowledgements");

    let mut payload = StoredDvzipReader::new(File::open(path)?, &file_name, plan)?;
    let mut buffer = [0_u8; STREAM_CHUNK_BYTES];
    let mut sent = 0_u64;
    let mut sequence = 0_u32;
    let mut next_report = 1024_u64 * 1024;
    loop {
        let count = payload.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        let chunk_crc32 = crc32fast::hash(&buffer[..count]);
        writeln!(
            port,
            "ESPDROP STREAM DATA {sequence} {count} {chunk_crc32:08x}"
        )?;
        port.flush()?;
        let ready = format!("ESPDROP-STREAM-DATA-READY seq={sequence}");
        wait_for_marker(port.as_mut(), ready.as_bytes(), Duration::from_secs(15))?;
        port.write_all(&buffer[..count])?;
        port.flush()?;
        let acknowledged = format!("ESPDROP-STREAM-ACK seq={sequence}");
        wait_for_marker(
            port.as_mut(),
            acknowledged.as_bytes(),
            Duration::from_secs(30),
        )?;
        sent += count as u64;
        sequence = sequence
            .checked_add(1)
            .ok_or("stream sequence number overflow")?;
        if sent >= next_report || sent == plan.payload_bytes {
            println!("streamed {sent}/{} payload bytes", plan.payload_bytes);
            next_report = sent + 1024 * 1024;
        }
    }
    if sent != plan.payload_bytes {
        return Err(format!(
            "generated stream ended at {sent} bytes; expected {}",
            plan.payload_bytes
        )
        .into());
    }
    println!("payload delivered to espDrop; waiting for the final AirDrop response");
    let completion = wait_for_stream_completion(port.as_mut(), Duration::from_secs(180))?;
    if completion.payload_bytes != plan.payload_bytes {
        return Err(format!(
            "AirDrop reported {} payload bytes; expected {}",
            completion.payload_bytes, plan.payload_bytes
        )
        .into());
    }
    println!(
        "AirDrop complete: HTTP {}, request={} bytes, payload={} bytes",
        completion.http_status, completion.request_bytes, completion.payload_bytes
    );
    Ok(())
}

fn upload(
    port_name: &str,
    expected_serial: &str,
    path: &Path,
    explicit_type: Option<&str>,
) -> Result<(), Box<dyn std::error::Error>> {
    let metadata = path.metadata()?;
    if !metadata.is_file() {
        return Err(format!("not a regular file: {}", path.display()).into());
    }
    if metadata.len() == 0 || metadata.len() > SPOOL_CAPACITY {
        return Err(format!(
            "file is {} bytes; this firmware accepts 1..={SPOOL_CAPACITY} bytes",
            metadata.len()
        )
        .into());
    }
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("the file name must be valid UTF-8")?;
    if file_name.len() > 127 || file_name.contains(['\n', '\r', '\0']) {
        return Err("the UTF-8 file name must be 1-127 bytes without control separators".into());
    }
    let file_type = explicit_type
        .or_else(|| inferred_type(path))
        .ok_or("unknown file extension; pass --type with an Apple UTI")?;
    if file_type.is_empty()
        || file_type.len() > 63
        || !file_type
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'.' || byte == b'-')
    {
        return Err(
            "file type must be a 1-63 byte UTI using letters, digits, dots, or hyphens".into(),
        );
    }

    let actual_serial = verify_target(port_name, expected_serial)?;
    let crc32 = file_crc32(path)?;
    println!(
        "verified {port_name}; Espressif serial/MAC={actual_serial}; staging {} ({} bytes, crc32={crc32:08x}, type={file_type})",
        path.display(),
        metadata.len()
    );

    let mut port = open_control_port(port_name)?;
    let name_hex: String = file_name
        .as_bytes()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    writeln!(
        port,
        "ESPDROP RELAY BEGIN {} {crc32:08x} {name_hex} {file_type}",
        metadata.len()
    )?;
    port.flush()?;
    wait_for_marker(
        port.as_mut(),
        b"ESPDROP-RELAY-READY",
        Duration::from_secs(90),
    )?;

    let mut source = File::open(path)?;
    let mut buffer = [0_u8; 4096];
    let mut sent = 0_u64;
    let mut next_report = 1024_u64 * 1024;
    loop {
        let count = source.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        port.write_all(&buffer[..count])?;
        sent += count as u64;
        if sent >= next_report || sent == metadata.len() {
            println!("staged {sent}/{} bytes", metadata.len());
            next_report = sent + 1024 * 1024;
        }
    }
    port.flush()?;
    wait_for_marker(
        port.as_mut(),
        b"ESPDROP-RELAY-STORED",
        Duration::from_secs(30),
    )?;
    println!("relay file stored and verified on espDrop");
    Ok(())
}

fn usage() -> &'static str {
    "Usage:\n  espdrop-cli ports\n  espdrop-cli ping --port <device> --serial <ESP serial/MAC>\n  espdrop-cli stats --port <device> --serial <ESP serial/MAC>\n  espdrop-cli wake --port <device> --serial <ESP serial/MAC>\n  espdrop-cli peers --port <device> --serial <ESP serial/MAC>\n  espdrop-cli target --port <device> --serial <ESP serial/MAC> --target <MAC|AUTO|NONE|ONLY>\n  espdrop-cli restart --port <device> --serial <ESP serial/MAC>\n  espdrop-cli send --port <device> --serial <ESP serial/MAC> --target <MAC|AUTO|ONLY> [--restart] [--type <UTI>] <file>\n  espdrop-cli upload --port <device> --serial <ESP serial/MAC> [--type <UTI>] <file>\n\n  ping     Verify serial control and report firmware uptime\n  stats    Report AWDL/TCP transport counters\n  wake     Re-arm the bounded AirDrop BLE wake window without rebooting\n  peers    List live AirDrop/AWDL receiver candidates discovered by the ESP\n  target   Select a temporary AWDL peer, automatic selection, no target, or the only live peer\n  restart  Reboot through USB and wait for serial control to return\n  send     Package on the host and stream after AirDrop acceptance\n  upload   Legacy: spool the raw file into ESP flash"
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let arguments: Vec<String> = env::args().skip(1).collect();
    match arguments.first().map(String::as_str) {
        Some("ports") if arguments.len() == 1 => ports(),
        Some(command @ ("ping" | "stats" | "wake" | "peers" | "target" | "restart")) => {
            let mut port = None;
            let mut serial = None;
            let mut target = None;
            let mut index = 1;
            while index < arguments.len() {
                let option = arguments[index].as_str();
                if !matches!(option, "--port" | "--serial" | "--target") {
                    return Err(format!("unknown option: {option}").into());
                }
                index += 1;
                let value = arguments
                    .get(index)
                    .ok_or_else(|| format!("missing value for {option}"))?
                    .clone();
                match option {
                    "--port" => port = Some(value),
                    "--serial" => serial = Some(value),
                    "--target" => target = Some(value),
                    _ => unreachable!(),
                }
                index += 1;
            }
            let port = port.ok_or("missing --port")?;
            let serial = serial.ok_or("missing --serial")?;
            match command {
                "ping" => {
                    if target.is_some() {
                        return Err("ping does not accept --target".into());
                    }
                    ping(&port, &serial)
                }
                "stats" => {
                    if target.is_some() {
                        return Err("stats does not accept --target".into());
                    }
                    stats(&port, &serial)
                }
                "wake" => {
                    if target.is_some() {
                        return Err("wake does not accept --target".into());
                    }
                    wake(&port, &serial)
                }
                "peers" => {
                    if target.is_some() {
                        return Err("peers does not accept --target".into());
                    }
                    list_peers(&port, &serial)
                }
                "target" => {
                    control_target(&port, &serial, target.as_deref().ok_or("missing --target")?)
                }
                "restart" => {
                    if target.is_some() {
                        return Err("restart does not accept --target".into());
                    }
                    restart(&port, &serial)
                }
                _ => unreachable!(),
            }
        }
        Some(command @ ("send" | "upload")) => {
            let mut port = None;
            let mut serial = None;
            let mut file_type = None;
            let mut target = None;
            let mut restart_first = false;
            let mut file = None;
            let mut index = 1;
            while index < arguments.len() {
                match arguments[index].as_str() {
                    "--port" | "--serial" | "--type" | "--target" => {
                        let option = arguments[index].as_str();
                        index += 1;
                        let value = arguments
                            .get(index)
                            .ok_or_else(|| format!("missing value for {option}"))?
                            .clone();
                        match option {
                            "--port" => port = Some(value),
                            "--serial" => serial = Some(value),
                            "--type" => file_type = Some(value),
                            "--target" => target = Some(value),
                            _ => unreachable!(),
                        }
                    }
                    "--restart" => restart_first = true,
                    value if value.starts_with('-') => {
                        return Err(format!("unknown option: {value}").into());
                    }
                    value if file.is_none() => file = Some(PathBuf::from(value)),
                    value => return Err(format!("unexpected argument: {value}").into()),
                }
                index += 1;
            }
            let port = port.ok_or("missing --port")?;
            let serial = serial.ok_or("missing --serial")?;
            let file = file.ok_or("missing file path")?;
            if command == "send" {
                send(
                    &port,
                    &serial,
                    &file,
                    file_type.as_deref(),
                    target.as_deref().ok_or("send requires --target")?,
                    restart_first,
                )
            } else {
                if target.is_some() || restart_first {
                    return Err("upload does not accept --target or --restart".into());
                }
                upload(&port, &serial, &file, file_type.as_deref())
            }
        }
        _ => Err(usage().into()),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {error}\n{}", usage());
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn prepared_payload_matches_c_core_fixture_shape() {
        let jpeg = [
            0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0xff, 0xd9,
        ];
        let plan = PreparedPlan::new(jpeg.len() as u64, "hello.jpg").unwrap();
        assert_eq!(plan.archive_bytes, 10_240);
        assert_eq!(plan.dvzip_blocks, 1);
        assert_eq!(plan.payload_bytes, 10_244);

        let mut payload = Vec::new();
        StoredDvzipReader::new(Cursor::new(jpeg), "hello.jpg", plan)
            .unwrap()
            .read_to_end(&mut payload)
            .unwrap();
        assert_eq!(payload.len() as u64, plan.payload_bytes);
        assert_eq!(&payload[..4], &[0x80, 0x00, 0x28, 0x00]);
        assert_eq!(&payload[4..10], b"070707");
        assert!(payload.windows(12).any(|value| value == b"./hello.jpg\0"));
        assert!(payload.windows(11).any(|value| value == b"TRAILER!!!\0"));
        assert_eq!(crc32fast::hash(&jpeg), 0xadb7_5530);
        assert_eq!(crc32fast::hash(&payload), 0xc808_29e8);
    }

    #[test]
    fn prepared_payload_inserts_headers_at_every_dvzip_boundary() {
        let file = vec![0x5a; 180_000];
        let plan = PreparedPlan::new(file.len() as u64, "relay.bin").unwrap();
        assert_eq!(plan.dvzip_blocks, 2);
        let mut payload = Vec::new();
        StoredDvzipReader::new(Cursor::new(file), "relay.bin", plan)
            .unwrap()
            .read_to_end(&mut payload)
            .unwrap();
        assert_eq!(payload.len() as u64, plan.payload_bytes);
        assert_eq!(&payload[..4], &[0x80, 0x02, 0x00, 0x00]);
        let second = 4 + DVZIP_BLOCK_BYTES as usize;
        let final_archive_block = plan.archive_bytes - DVZIP_BLOCK_BYTES;
        assert_eq!(
            &payload[second..second + 4],
            &(0x8000_0000_u32 | final_archive_block as u32).to_be_bytes()
        );
    }

    #[test]
    fn serial_normalization_ignores_common_separators() {
        assert_eq!(normalized_serial("1c:db-d4:42:3f:a0"), "1CDBD4423FA0");
    }

    #[test]
    fn peer_targets_are_canonicalized_for_the_serial_protocol() {
        assert_eq!(
            validate_peer_target("52-F4-36-B8-FD-F5").unwrap(),
            "52:f4:36:b8:fd:f5"
        );
        assert_eq!(validate_peer_target("auto").unwrap(), "AUTO");
        assert!(validate_peer_target("52:f4:36").is_err());
    }

    #[test]
    fn final_stream_result_requires_successful_upload() {
        assert_eq!(
            parse_stream_result(
                "ESPDROP-STREAM-RESULT state=pending stage=none error=0 status=0 request_bytes=0 payload_bytes=0"
            )
            .unwrap(),
            None
        );
        assert_eq!(
            parse_stream_result(
                "ESPDROP-STREAM-RESULT state=success stage=upload error=0 status=200 request_bytes=62001 payload_bytes=61444"
            )
            .unwrap(),
            Some(StreamCompletion {
                http_status: 200,
                request_bytes: 62_001,
                payload_bytes: 61_444,
            })
        );
        assert!(parse_stream_result(
            "ESPDROP-STREAM-RESULT state=failed stage=upload error=-1 status=0 request_bytes=62001 payload_bytes=61444"
        )
        .is_err());
    }

    #[test]
    fn complete_peer_records_expose_their_session_mac() {
        let complete = "ESPDROP-PEER mac=72:ac:06:9c:da:fe rssi=-30 signals=6 valid=1 airdrop=1 class=1 distance=0 endpoint=1 port=8770 age_ms=1 airdrop_age_ms=1 instance=test";
        let raw = "ESPDROP-PEER mac=52:f4:36:b8:fd:f5 rssi=-20 signals=2 valid=1 airdrop=0 class=1 distance=0 endpoint=0 port=0 age_ms=1 airdrop_age_ms=18446744073709551615 instance=-";
        let stale = "ESPDROP-PEER mac=e2:2d:c5:74:c3:36 rssi=-40 signals=2 valid=1 airdrop=0 class=1 distance=0 endpoint=0 port=0 age_ms=5001 airdrop_age_ms=18446744073709551615 instance=-";
        let stale_complete = "ESPDROP-PEER mac=72:ac:06:9c:da:fe rssi=-30 signals=6 valid=1 airdrop=1 class=1 distance=0 endpoint=1 port=8770 age_ms=1 airdrop_age_ms=5001 instance=test";
        assert_eq!(
            peer_mac_from_line(complete, true),
            Some("72:ac:06:9c:da:fe")
        );
        assert_eq!(peer_mac_from_line(raw, true), None);
        assert_eq!(peer_mac_from_line(raw, false), Some("52:f4:36:b8:fd:f5"));
        assert_eq!(peer_mac_from_line(stale, false), None);
        assert_eq!(peer_mac_from_line(stale_complete, true), None);
        assert_eq!(validate_peer_target("only").unwrap(), "ONLY");
    }

    #[test]
    fn common_airdrop_types_are_inferred() {
        assert_eq!(inferred_type(Path::new("photo.JPG")), Some("public.jpeg"));
        assert_eq!(inferred_type(Path::new("scan.pdf")), Some("com.adobe.pdf"));
    }
}
