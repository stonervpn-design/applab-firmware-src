#pragma once

typedef enum {
    SubGhzCustomEventManagerNoSet = 0,
    SubGhzCustomEventManagerSet,
    SubGhzCustomEventManagerSetRAW,

    //SubGhzCustomEvent
    SubGhzCustomEventSceneDeleteSuccess = 100,
    SubGhzCustomEventSceneDelete,
    SubGhzCustomEventSceneDeleteBack,
    SubGhzCustomEventSceneDeleteRAW,
    SubGhzCustomEventSceneDeleteRAWBack,

    SubGhzCustomEventSceneReceiverInfoTxStart,
    SubGhzCustomEventSceneReceiverInfoTxStop,
    SubGhzCustomEventSceneReceiverInfoSave,
    SubGhzCustomEventSceneReceiverInfoAnalyze,
    SubGhzCustomEventSceneSaveName,
    SubGhzCustomEventSceneSignalSettings,
    SubGhzCustomEventSceneSaveSuccess,
    SubGhzCustomEventSceneShowErrorBack,
    SubGhzCustomEventSceneShowErrorOk,
    SubGhzCustomEventSceneShowErrorSub,
    SubGhzCustomEventSceneShowOnlyRX,
    SubGhzCustomEventSceneAnalyzerLock,
    SubGhzCustomEventSceneAnalyzerUnlock,
    SubGhzCustomEventSceneSettingLock,
    SubGhzCustomEventSceneSettingResetToDefault,

    SubGhzCustomEventSceneExit,
    SubGhzCustomEventSceneStay,

    SubGhzCustomEventSceneRpcLoad,
    SubGhzCustomEventSceneRpcButtonPress,
    SubGhzCustomEventSceneRpcButtonRelease,
    SubGhzCustomEventSceneRpcButtonPressRelease,
    SubGhzCustomEventSceneRpcSessionClose,

    SubGhzCustomEventViewReceiverOK,
    SubGhzCustomEventViewReceiverConfig,
    SubGhzCustomEventViewReceiverBack,
    SubGhzCustomEventViewReceiverOffDisplay,
    SubGhzCustomEventViewReceiverUnlock,
    SubGhzCustomEventViewReceiverDeleteItem,

    SubGhzCustomEventViewReadRAWBack,
    SubGhzCustomEventViewReadRAWIDLE,
    SubGhzCustomEventViewReadRAWREC,
    SubGhzCustomEventViewReadRAWConfig,
    SubGhzCustomEventViewReadRAWErase,
    SubGhzCustomEventViewReadRAWSendStart,
    SubGhzCustomEventViewReadRAWSendStop,
    SubGhzCustomEventViewReadRAWSave,
    SubGhzCustomEventViewReadRAWTXRXStop,
    SubGhzCustomEventViewReadRAWMore,

    SubGhzCustomEventViewTransmitterBack,
    SubGhzCustomEventViewTransmitterSendStart,
    SubGhzCustomEventViewTransmitterSendStop,
    SubGhzCustomEventViewTransmitterError,

    SubGhzCustomEventViewFreqAnalOkShort,
    SubGhzCustomEventViewFreqAnalOkLong,

    SubGhzCustomEventByteInputDone,
    SubGhzCustomEventNumberInputDone,
    SubGhzCustomEventTpmsEditPressure,
    SubGhzCustomEventTpmsEditTemperature,
    SubGhzCustomEventTpmsEditId,
    SubGhzCustomEventTpmsToggleBattery,

    SubGhzCustomEventSceneRollingAnalyzerClone,
    SubGhzCustomEventSceneRollingAnalyzerSave, /* Rolling-Code Analyzer -> KeeLoq no-desync clone */
    SubGhzCustomEventSceneRollingAnalyzerNext, /* keyless-predictable fob -> synthesize next code */
    SubGhzCustomEventSceneRollingAnalyzerBurst, /* -> next N consecutive codes in one action */
    SubGhzCustomEventSceneRollingAnalyzerForge, /* Hitag2 -> known-key counter-recovery + forge */

    SubGhzCustomEventSceneRollbackUpdate,
    SubGhzCustomEventSceneRollbackDone,

    SubGhzCustomEventViewProtoviewFreeze,
    SubGhzCustomEventViewProtoviewDecode,
    SubGhzCustomEventViewProtoviewEdit,
    SubGhzCustomEventViewProtoviewReplay,
    SubGhzCustomEventViewProtoviewSave,
    SubGhzCustomEventViewProtoviewBack,

    SubGhzCustomEventScenePsaBfTick, /* PSA brute-force progress tick */

    SubGhzCustomEventSceneAnalyzeDetails, /* Signal-ID -> deeper tool (rolling/PSA) */
    SubGhzCustomEventSceneAnalyzeAttack, /* Signal-ID -> guided SubBrute (fixed-code, private) */
    SubGhzCustomEventSceneAnalyzeEmulate, /* Signal-ID -> multi-button rolling fob grid (supported car fob) */

    SubGhzCustomEventViewKeeloqFobSend,
    SubGhzCustomEventViewKeeloqFobBack,

    SubGhzCustomEventViewSpectrumTune, /* Spectrum -> tune receiver to peak (private) */
    SubGhzCustomEventViewSpectrumBandUp,
    SubGhzCustomEventViewSpectrumBandDown,
    SubGhzCustomEventViewSpectrumBack,

#ifndef PUBLIC_BUILD
    /* Protocol Fuzzer run view -> fuzzer_worker start/pause/step/stop/mark
     * (private). Translated 1:1 from SubGhzViewFuzzerEvent by the run
     * scene's view callback -- kept as real SubGhzCustomEvent members
     * (rather than sending the raw SubGhzViewFuzzerEvent 0-3 values) so they
     * live in this enum's numbering space instead of colliding with
     * SubGhzCustomEventManagerNoSet/Set/SetRAW (0-2). */
    SubGhzCustomEventViewFuzzerStartPause,
    SubGhzCustomEventViewFuzzerStep,
    SubGhzCustomEventViewFuzzerMark,
    SubGhzCustomEventViewFuzzerStop,
#endif
} SubGhzCustomEvent;

typedef enum {
    SetTypeFaacSLH_868,
    SetTypeFaacSLH_433,
    SetTypeBFTMitto,
    SetTypeSomfyTelis,
    SetTypeSomfyKeytis,
    SetTypeKingGatesStylo4k,
    SetTypeBenincaARC,
    SetTypeJarolift,
    SetTypeDitecGOL4,
    SetTypeANMotorsAT4,
    SetTypeAlutechAT4N,
    SetTypePhoenix_V2_433,
    SetTypeHCS101_433_92,
    SetTypeDoorHan_315_00,
    SetTypeDoorHan_433_92,
    SetTypeBeninca433,
    SetTypeBeninca868,
    SetTypeComunello433,
    SetTypeComunello868,
    SetTypeAllmatic433,
    SetTypeAllmatic868,
    SetTypeCenturion433,
    SetTypeMonarch433,
    SetTypeJollyMotors433,
    SetTypeMotorline433,
    SetTypeSommer_FM_434,
    SetTypeSommer_FM_868,
    SetTypeSommer_FM12K_434,
    SetTypeSommer_FM12K_868,
    SetTypeStilmatic,
    SetTypeIronLogic,
    SetTypeIronLogicSmart,
    SetTypeDeaMio433,
    SetTypeDTMNeo433,
    SetTypeGibidi433,
    SetTypeGSN,
    SetTypeAprimatic,
    SetTypeElmesElectronic,
    SetTypeNormstahl_433_92,
    SetTypeJCM_433_92,
    SetTypeNovoferm_433_92,
    SetTypeHormannEcoStar_433_92,
    SetTypeCardinS449_433FM,
    SetTypeFAACRCXT_433_92,
    SetTypeFAACRCXT_868,
    SetTypeGeniusBravo433,
    SetTypeNiceMHouse_433_92,
    SetTypeNiceSmilo_433_92,
    SetTypeNiceFlorS_433_92,
    SetTypeNiceOne_433_92,
    SetTypeNiceFlo12bit,
    SetTypeNiceFlo24bit,
    SetTypeCAME12bit,
    SetTypeCAME24bit,
    SetTypeCAME12bit868,
    SetTypeCAME24bit868,
    SetTypeCAMETwee,
    SetTypeCameAtomo433,
    SetTypeCameAtomo868,
    SetTypeCAMESpace,
    SetTypePricenton315,
    SetTypePricenton433,
    SetTypeZKTeco430,
    SetTypeBETT_433,
    SetTypeGangQi_433,
    SetTypeHollarm_433,
    SetTypeReversRB2_433,
    SetTypeMarantec24_868,
    SetTypeMarantec_433,
    SetTypeMarantec_868,
    SetTypeRoger_433,
    SetTypeLinear_300_00,
    // SetTypeNeroSketch, //Deleted in OFW
    // SetTypeNeroRadio, //Deleted in OFW
    SetTypeGateTX,
    SetTypeSecPlus_v1_315_00,
    SetTypeSecPlus_v1_390_00,
    SetTypeSecPlus_v1_433_00,
    SetTypeSecPlus_v2_310_00,
    SetTypeSecPlus_v2_315_00,
    SetTypeSecPlus_v2_390_00,
    SetTypeSecPlus_v2_433_00,

    SetTypeKeeloqByMfr,

    SetTypeMAX,
} SetType;
