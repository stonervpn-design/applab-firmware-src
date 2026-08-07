// RF campaign runner (PRIVATE builds): sweep a band with the native auto-freq-
// finder, then tune the receiver to the strongest carrier. subghz.sweep() does
// the whole retune+RSSI pass in C, so the device runs the campaign standalone --
// no host/PC scripting API needed.
checkSdkFeatures(["subghz"]);

let subghz = require("subghz");
subghz.setup();

let bandStart = 433050000; // 433.05 MHz
let bandEnd = 434790000; // 434.79 MHz

print("Sweeping", bandStart, "-", bandEnd, "...");
let r = subghz.sweep(bandStart, bandEnd, 128); // { peakFreq, peakRssi, bins:[dBm...] }
print("Peak:", r.peakFreq, "Hz  RSSI:", r.peakRssi, "dBm  (", r.bins.length, "bins)");

if (r.peakRssi > -85) {
    print("Signal found -- tuning receiver to the peak.");
    subghz.setIdle();
    subghz.setFrequency(r.peakFreq);
    subghz.setRx();
    // ... capture / decode / act here ...
    delay(2000);
    print("Live RSSI at peak:", subghz.getRssi(), "dBm");
    subghz.setIdle();
} else {
    print("Band quiet -- nothing above -85 dBm.");
}

subghz.end();
