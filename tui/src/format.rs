pub fn format_price(mantissa: i64, scale: u8) -> String {
    if scale == 0 {
        return mantissa.to_string();
    }
    let neg = mantissa < 0;
    let digits = mantissa.unsigned_abs().to_string();
    let scale = scale as usize;
    let (int_part, frac_part) = if digits.len() > scale {
        let split = digits.len() - scale;
        (digits[..split].to_string(), digits[split..].to_string())
    } else {
        ("0".to_string(), format!("{digits:0>scale$}"))
    };
    format!("{}{}.{}", if neg { "-" } else { "" }, int_part, frac_part)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scale_zero_is_integer() {
        assert_eq!(format_price(1234, 0), "1234");
        assert_eq!(format_price(-7, 0), "-7");
    }

    #[test]
    fn scale_two_places_decimal() {
        assert_eq!(format_price(58050, 2), "580.50");
        assert_eq!(format_price(100, 2), "1.00");
    }

    #[test]
    fn mantissa_shorter_than_scale_pads_zeros() {
        assert_eq!(format_price(5, 2), "0.05");
        assert_eq!(format_price(5, 4), "0.0005");
    }

    #[test]
    fn negative_mantissa() {
        assert_eq!(format_price(-58050, 2), "-580.50");
        assert_eq!(format_price(-5, 2), "-0.05");
    }

    #[test]
    fn large_values() {
        assert_eq!(format_price(123456789, 4), "12345.6789");
    }

    #[test]
    fn zero() {
        assert_eq!(format_price(0, 2), "0.00");
    }
}
