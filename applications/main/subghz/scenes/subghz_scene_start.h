#pragma once

enum SubmenuIndex {
    SubmenuIndexRead = 10,
    SubmenuIndexSaved,
    SubmenuIndexAddManually,
    SubmenuIndexAddManuallyAdvanced,
    SubmenuIndexFrequencyAnalyzer,
    SubmenuIndexFobAnalyzer,
    SubmenuIndexReadRAW,
    SubmenuIndexJammer,
    SubmenuIndexBruteforce,
    SubmenuIndexExtSettings,
    SubmenuIndexRadioSetting,
    SubmenuIndexPlaylist,
    SubmenuIndexVehicleVulns,
    SubmenuIndexToolkit,
    SubmenuIndexRollBack,
    SubmenuIndexSignalDecoder,
    SubmenuIndexCarKeys,
    SubmenuIndexSpectrum,
    SubmenuIndexResumeBrute,
    SubmenuIndexTpmsSpoof,
#ifndef PUBLIC_BUILD
    SubmenuIndexProtocolFuzzer,
#endif
};
