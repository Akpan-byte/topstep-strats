// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Re-exported C ABI strategy types from the lightweight engine_abi crate.
//   - Kept the CStrategyAdapter that routes the Strategy trait through a C ABI vtable.
// WHY: Strategy plugins should not need to link PyO3/Arrow/Parquet just to share Bar/Signal/VTable.

pub use engine_abi::{Bar, Signal, Strategy, StrategyVTable};

/// Adapter that routes the Rust Strategy trait through a C ABI vtable.
pub struct CStrategyAdapter {
    handle: *mut (),
    vtable: &'static StrategyVTable,
}

impl CStrategyAdapter {
    /// # Safety
    /// The vtable must remain valid for the lifetime of the adapter.
    pub unsafe fn new(handle: *mut (), vtable: &'static StrategyVTable) -> Self {
        CStrategyAdapter { handle, vtable }
    }
}

impl Drop for CStrategyAdapter {
    fn drop(&mut self) {
        (self.vtable.destroy)(self.handle);
    }
}

impl Strategy for CStrategyAdapter {
    fn prepare(&mut self, bars: &[Bar]) {
        if let Some(prepare) = self.vtable.prepare {
            prepare(self.handle, bars.as_ptr(), bars.len());
        }
    }

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
        let mut out = Signal::default();
        (self.vtable.on_bar)(self.handle, bar, &mut out);
        if out.direction == 0 {
            None
        } else {
            Some(out)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct BuyAndHold;
    impl Strategy for BuyAndHold {
        fn on_bar(&mut self, _bar: &Bar) -> Option<Signal> {
            Some(Signal {
                direction: 1,
                entry_price: 0.0,
                stop_loss: 0.0,
                take_profit: 0.0,
            })
        }
    }

    #[test]
    fn test_trait_signal() {
        let bar = Bar {
            timestamp_ns: 0,
            open: 1.0,
            high: 2.0,
            low: 0.5,
            close: 1.5,
            volume: 1,
        };
        let mut s = BuyAndHold;
        let sig = s.on_bar(&bar).unwrap();
        assert_eq!(sig.direction, 1);
    }
}
