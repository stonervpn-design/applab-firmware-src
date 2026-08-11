#pragma once

// Custom events for the Camera Hunter scene manager. Values above the results
// index range so they never collide with a findings-submenu index.
typedef enum {
    CamHunterEvtAuto = 200,
    CamHunterEvtResults,
    CamHunterEvtAuthorize,
    CamHunterEvtAgree,
    CamHunterEvtHuntFinished,
    CamHunterEvtAuditCam, // "Audit this camera" pressed on a detected own-AP cam
} CamHunterEvent;
