// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Re-exported Bar/ReplayIter from engine_abi.
//   - Moved CSV/Parquet loaders to free functions so Bar can stay in the ABI crate.
// WHY: Keep the canonical bar type lightweight and shareable across languages.

use std::error::Error;
use std::path::Path;

use arrow::array::{Array, ArrayRef, Float64Array, Int64Array, UInt64Array};
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

pub use engine_abi::{Bar, ReplayIter};

/// Load bars from a CSV with header:
///   timestamp_ns,open,high,low,close,volume
/// Extra columns are ignored. Rows are sorted by timestamp to guarantee chronological replay.
pub fn load_csv<P: AsRef<Path>>(path: P) -> Result<Vec<Bar>, Box<dyn Error>> {
    let mut rdr = csv::ReaderBuilder::new().has_headers(true).from_path(path)?;
    let mut bars = Vec::new();
    for result in rdr.records() {
        let record = result?;
        let parse_i64 = |idx: usize| record.get(idx).and_then(|s| s.parse::<i64>().ok());
        let parse_f64 = |idx: usize| record.get(idx).and_then(|s| s.parse::<f64>().ok());
        let parse_u64 = |idx: usize| record.get(idx).and_then(|s| s.parse::<u64>().ok());
        if let (Some(ts), Some(o), Some(h), Some(l), Some(c), Some(v)) = (
            parse_i64(0),
            parse_f64(1),
            parse_f64(2),
            parse_f64(3),
            parse_f64(4),
            parse_u64(5),
        ) {
            bars.push(Bar {
                timestamp_ns: ts,
                open: o,
                high: h,
                low: l,
                close: c,
                volume: v,
            });
        }
    }
    bars.sort_by_key(|b| b.timestamp_ns);
    Ok(bars)
}

/// Load bars from a Parquet file containing the canonical OHLCV columns.
/// Supported column names: timestamp_ns, open, high, low, close, volume.
/// `timestamp_ns` must be Int64; OHLC must be Float64; volume may be Int64 or UInt64.
pub fn load_parquet<P: AsRef<Path>>(path: P) -> Result<Vec<Bar>, Box<dyn Error>> {
    let file = std::fs::File::open(path)?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file)?;
    let reader = builder.build()?;

    let mut bars = Vec::new();
    for batch_result in reader {
        let batch = batch_result?;
        let ts_col = require_column(&batch, "timestamp_ns")?;
        let open_col = require_column(&batch, "open")?;
        let high_col = require_column(&batch, "high")?;
        let low_col = require_column(&batch, "low")?;
        let close_col = require_column(&batch, "close")?;
        let volume_col = require_column(&batch, "volume")?;

        let ts_arr = as_int64_array(ts_col, "timestamp_ns")?;
        let open_arr = as_float64_array(open_col, "open")?;
        let high_arr = as_float64_array(high_col, "high")?;
        let low_arr = as_float64_array(low_col, "low")?;
        let close_arr = as_float64_array(close_col, "close")?;
        let volume_arr = as_u64_array(volume_col)?;

        for i in 0..batch.num_rows() {
            if ts_arr.is_null(i)
                || open_arr.is_null(i)
                || high_arr.is_null(i)
                || low_arr.is_null(i)
                || close_arr.is_null(i)
                || volume_arr.is_null(i)
            {
                continue;
            }
            bars.push(Bar {
                timestamp_ns: ts_arr.value(i),
                open: open_arr.value(i),
                high: high_arr.value(i),
                low: low_arr.value(i),
                close: close_arr.value(i),
                volume: volume_arr.value(i),
            });
        }
    }

    bars.sort_by_key(|b| b.timestamp_ns);
    Ok(bars)
}

fn require_column<'a>(
    batch: &'a arrow::record_batch::RecordBatch,
    name: &str,
) -> Result<&'a ArrayRef, Box<dyn Error>> {
    batch
        .column_by_name(name)
        .ok_or_else(|| format!("missing required column: {}", name).into())
}

fn as_int64_array<'a>(col: &'a ArrayRef, name: &str) -> Result<&'a Int64Array, Box<dyn Error>> {
    col.as_any()
        .downcast_ref::<Int64Array>()
        .ok_or_else(|| format!("column {} must be Int64", name).into())
}

fn as_float64_array<'a>(
    col: &'a ArrayRef,
    name: &str,
) -> Result<&'a Float64Array, Box<dyn Error>> {
    col.as_any()
        .downcast_ref::<Float64Array>()
        .ok_or_else(|| format!("column {} must be Float64", name).into())
}

enum VolumeArray<'a> {
    U64(&'a UInt64Array),
    I64(&'a Int64Array),
}

impl VolumeArray<'_> {
    fn is_null(&self, i: usize) -> bool {
        match self {
            VolumeArray::U64(a) => a.is_null(i),
            VolumeArray::I64(a) => a.is_null(i),
        }
    }

    fn value(&self, i: usize) -> u64 {
        match self {
            VolumeArray::U64(a) => a.value(i),
            VolumeArray::I64(a) => a.value(i) as u64,
        }
    }
}

fn as_u64_array(col: &ArrayRef) -> Result<VolumeArray<'_>, Box<dyn Error>> {
    if let Some(arr) = col.as_any().downcast_ref::<UInt64Array>() {
        return Ok(VolumeArray::U64(arr));
    }
    if let Some(arr) = col.as_any().downcast_ref::<Int64Array>() {
        return Ok(VolumeArray::I64(arr));
    }
    Err(format!(
        "volume column must be UInt64 or Int64, got {:?}",
        col.data_type()
    )
    .into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::NamedTempFile;

    #[test]
    fn test_csv_roundtrip_and_order() {
        let mut file = NamedTempFile::new().unwrap();
        writeln!(
            file,
            "timestamp_ns,open,high,low,close,volume\n300,3.0,3.5,2.9,3.2,100\n100,1.0,1.5,0.9,1.2,50\n200,2.0,2.5,1.9,2.2,75"
        )
        .unwrap();
        let bars = load_csv(file.path()).unwrap();
        assert_eq!(bars.len(), 3);
        assert_eq!(bars[0].timestamp_ns, 100);
        assert_eq!(bars[2].timestamp_ns, 300);
    }

    #[test]
    fn test_parquet_roundtrip_and_order() {
        use arrow::array::{Float64Array, Int64Array, UInt64Array};
        use arrow::datatypes::{DataType, Field, Schema};
        use arrow::record_batch::RecordBatch;
        use parquet::arrow::arrow_writer::ArrowWriter;
        use std::sync::Arc;

        let schema = Arc::new(Schema::new(vec![
            Field::new("timestamp_ns", DataType::Int64, false),
            Field::new("open", DataType::Float64, false),
            Field::new("high", DataType::Float64, false),
            Field::new("low", DataType::Float64, false),
            Field::new("close", DataType::Float64, false),
            Field::new("volume", DataType::UInt64, false),
        ]));

        // Rows intentionally out of order to exercise sorting.
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int64Array::from(vec![300, 100, 200])),
                Arc::new(Float64Array::from(vec![3.0, 1.0, 2.0])),
                Arc::new(Float64Array::from(vec![3.5, 1.5, 2.5])),
                Arc::new(Float64Array::from(vec![2.9, 0.9, 1.9])),
                Arc::new(Float64Array::from(vec![3.2, 1.2, 2.2])),
                Arc::new(UInt64Array::from(vec![100, 50, 75])),
            ],
        )
        .unwrap();

        let mut temp = NamedTempFile::new().unwrap();
        {
            let mut writer = ArrowWriter::try_new(&mut temp, schema, None).unwrap();
            writer.write(&batch).unwrap();
            writer.close().unwrap();
        }

        let bars = load_parquet(temp.path()).unwrap();
        assert_eq!(bars.len(), 3);
        assert_eq!(bars[0].timestamp_ns, 100);
        assert_eq!(bars[1].timestamp_ns, 200);
        assert_eq!(bars[2].timestamp_ns, 300);
        assert!((bars[2].close - 3.2).abs() < 1e-9);
    }
}
