use blip25_vocoder::halfrate::frame::{
    decode_code_vectors, encode_code_vectors, CODE_WIDTHS,
};
use blip25_vocoder::vocoder::{FrameStatus, Rate, Vocoder, FRAME_SAMPLES};

fn tone_440() -> [i16; FRAME_SAMPLES] {
    core::array::from_fn(|i| {
        let t = i as f32 / 8000.0;
        (7000.0 * (2.0 * core::f32::consts::PI * 440.0 * t).sin()) as i16
    })
}

fn code_vectors_to_bytes(code: &[u32; 4]) -> [u8; 9] {
    let mut out = [0u8; 9];
    let mut bit = 0usize;

    for (i, &word) in code.iter().enumerate() {
        for k in (0..CODE_WIDTHS[i] as usize).rev() {
            let v = ((word >> k) & 1) as u8;
            if v != 0 {
                out[bit >> 3] |= 1 << (7 - (bit & 7));
            }
            bit += 1;
        }
    }

    assert_eq!(bit, 72);
    out
}

fn bytes_to_code_vectors(bytes: &[u8; 9]) -> [u32; 4] {
    let mut bit = 0usize;

    core::array::from_fn(|i| {
        let mut word = 0u32;
        for _ in 0..CODE_WIDTHS[i] {
            let v = (bytes[bit >> 3] >> (7 - (bit & 7))) & 1;
            word = (word << 1) | u32::from(v);
            bit += 1;
        }
        word
    })
}

fn main() {
    let pcm = tone_440();

    let mut enc = Vocoder::new(Rate::HalfRate3600x2450);
    let info = enc.encode_info(&pcm).expect("encode_info");
    let info: [u16; 4] = info.try_into().expect("4 half-rate vectors");

    let code = encode_code_vectors(&info);
    let canonical = code_vectors_to_bytes(&code);

    let decoded_fec = decode_code_vectors(bytes_to_code_vectors(&canonical));
    assert_eq!(decoded_fec.info, info, "clean FEC round-trip");

    let mut dec = Vocoder::new(Rate::HalfRate3600x2450);
    let out = dec
        .decode_info(&decoded_fec.info, FrameStatus::CLEAN)
        .expect("decode_info");

    assert_eq!(out.len(), FRAME_SAMPLES);

    let input_energy: u64 = pcm.iter().map(|&v| i64::from(v).unsigned_abs()).sum();
    let output_energy: u64 = out.iter().map(|&v| i64::from(v).unsigned_abs()).sum();

    assert!(input_energy > 0);
    assert!(output_energy > 0, "decoded audio should not be silent");

    print!("canonical72=");
    for b in canonical {
        print!("{b:02x}");
    }
    println!();

    println!(
        "roundtrip=OK corrected={} input_energy={} output_energy={}",
        decoded_fec.error_total(),
        input_energy,
        output_energy
    );
}
