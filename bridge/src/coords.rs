use serde::{Deserialize, Serialize};

pub const METRES_PER_YARD: f64 = 0.9144;
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Point(pub f64, pub f64, pub f64);
impl Point {
    pub fn valid(self) -> bool {
        [self.0, self.1, self.2]
            .iter()
            .all(|v| v.is_finite() && v.abs() <= 1_000_000.0)
    }
}
/// Canonical bridge coordinates: right handed, Y up, metres. The BG3 side must calibrate BG3's basis.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Basis {
    pub wow_origin: Point,
    pub bridge_origin: Point,
    pub yaw: f64,
}
impl Basis {
    pub fn valid(self) -> bool {
        self.wow_origin.valid() && self.bridge_origin.valid() && self.yaw.is_finite()
    }
    pub fn to_bridge(self, p: Point) -> Point {
        let x = -(p.1 - self.wow_origin.1) * METRES_PER_YARD;
        let y = (p.2 - self.wow_origin.2) * METRES_PER_YARD;
        let z = -(p.0 - self.wow_origin.0) * METRES_PER_YARD;
        let (s, c) = self.yaw.sin_cos();
        Point(
            self.bridge_origin.0 + c * x + s * z,
            self.bridge_origin.1 + y,
            self.bridge_origin.2 - s * x + c * z,
        )
    }
    pub fn to_wow(self, p: Point) -> Point {
        let x = p.0 - self.bridge_origin.0;
        let y = p.1 - self.bridge_origin.1;
        let z = p.2 - self.bridge_origin.2;
        let (s, c) = self.yaw.sin_cos();
        Point(
            self.wow_origin.0 - (s * x + c * z) / METRES_PER_YARD,
            self.wow_origin.1 - (c * x - s * z) / METRES_PER_YARD,
            self.wow_origin.2 + y / METRES_PER_YARD,
        )
    }
}
