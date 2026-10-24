# iNaturalist computer-vision suggestions in Review Unmatched

## Context

Reviewing unmatched photos means finding the taxon by hand. iNaturalist's computer-vision model
(the "suggestions" its apps show) could propose IDs from the photo itself. The endpoint,
`POST /v1/computervision/score_image`, exists but is **not a public API**: staff say it's meant for
iNaturalist's own apps, access for third parties is granted case by case, and unapproved
third-party use has been getting 403 blocks (June 2026). PhotoLife is public open source.

User decisions:
- Use iNat CV, **after asking iNaturalist for permission**; build the feature now using the user's
  own iNat API token, treated as experimental, handling refusal (403) clearly.
- Results: a **ranked list the user confirms from** (no automatic decisions).

User action (outside the code): register PhotoLife at https://www.inaturalist.org/oauth/applications
(needs ~10 "improving" identifications on the account) and email help@inaturalist.org asking for
computer-vision access for PhotoLife. The feature is built so it works as soon as access is allowed.

Alternative discussed (no registration): the call already authenticates with the user's own pasted
iNat API token (the same "login" the iNaturalist download uses) — registration only concerns
iNaturalist's *permission*, not the mechanics. So it can ship as an **opt-in setting, off by
default**, in iNaturalist Tools: "Use iNaturalist's computer vision for suggestions (unofficial —
iNaturalist may block it)". Settings key e.g. `inat/visionEnabled`; the Review button is hidden or
disabled (with a tooltip pointing to the setting) until it's on. Requests go out as the user's own
account, one per click; a 403 explains and nothing else breaks. Asking iNaturalist can still follow.

## Learned from WilliamsJason/inat-lightroom (MIT, Lua; docs/inat-api-notes.md, InatAPI.lua)
- `score_image` works with the user's own 24 h JWT sent as `Authorization: Bearer <jwt>`.
  A plain (non-expiring) OAuth token is NOT rejected — the call silently runs as anonymous.
- **lat / lng / observed_on must be multipart form fields** (in the query string they're silently
  ignored and every `frequency_score` is 0). Image as part `image`, filename + `image/jpeg`.
- 1024 px JPEG is plenty.
- Response: `results[]{combined_score, vision_score, frequency_score, taxon{id,name,rank,
  preferred_common_name}}` + optional `common_ancestor{score, taxon}` — no photo/ancestry, so the
  reference photo needs `fetchTaxon` (the Review pane already does this on selection).
- Show `common_ancestor` as a clickable "probably within <genus/family>" row: an honest coarser ID
  when species-level candidates disagree.
- Optional follow-up (separate feature): proper **Sign In with iNaturalist** via OAuth
  authorization code + PKCE (no secret; public client_id), a never-expiring OAuth token stored once,
  and 24 h JWTs minted automatically from `GET https://www.inaturalist.org/users/api_token`
  (Bearer <oauth token>) — ends the daily token paste for downloads too. Needs PhotoLife's own
  registered iNat app (manual approval; account ≥ 2 months, ≥ 10 improving IDs for others in the
  last month); redirect via loopback like `libsync/GoogleAuth`. Token exchange: POST /oauth/token,
  form-encoded, no client_secret. Don't reuse their client_id.

## Behaviour

- Review Unmatched: a **Suggest IDs (iNaturalist)** button beside the "Choose the taxon" heading
  (shortcut Ctrl+Shift+I). It sends the current photo (a ~1024 px JPEG; RAW-only shots use the
  embedded preview) plus its location and date if known, so "seen nearby" works. It is scored, not
  posted as an observation.
- Results appear in a compact list above the candidates, up to 8 rows, best first:
  `Flame Robin · Petroica phoenicea [species] — 94% · seen nearby · in your trees`
  (not-in-a-tree rows greyed, labelled "not in your trees"). A first line gives iNat's
  "probably within" common ancestor when it sends one (e.g. "Petroica (genus) 97%").
- Clicking a suggestion puts that taxon at the top of the candidate list and selects it — so the
  reference photo, Confirm, Genus only and Apply to folder all work exactly as now. Nothing is
  confirmed automatically.
- Results are kept per photo for the session (moving back doesn't re-ask); moving to another photo
  cancels/ignores a pending reply for the previous one.
- Errors, in the list area, each with what to do:
  - no token → "Sign in: paste your iNaturalist API token in iNaturalist Tools" (button jumps there);
  - 401 → "Your iNaturalist token has expired (they last a day) — paste a fresh one";
  - 403 → "iNaturalist hasn't allowed PhotoLife to use its computer vision (yet), or is limiting
    requests" + link explaining (help section);
  - network/other → the error text.
- Tooltip and help make clear the photo is sent to iNaturalist for scoring.

## Implementation

Networking (POST support, reusing the rate limiter):
- `src/net/Transport.h`: `Request` gains `verb` (default "GET"), `body`, `contentType`.
- `src/net/QtNetworkTransport.cpp`: non-GET → `m_nam->sendCustomRequest(req, verb, body)` with
  Content-Type; GET unchanged.
- `src/net/HttpClient.{h,cpp}`: `post(url, body, contentType, handler, bearerToken, priority)`
  through the same queue / min interval / 429-5xx retry; never cached (bypasses `HttpCache`).
- `src/net/INatClient.{h,cpp}`: `scoreImage(QByteArray jpeg, std::optional<QPointF> latLng,
  std::optional<QDate> observedOn, handler(Outcome<VisionResult>))` — hand-built
  `multipart/form-data` (boundary via QUuid, as `GoogleDriveStore` does for multipart/related):
  fields `image` (filename photo.jpg, image/jpeg), `lat`, `lng`, `observed_on`; uses
  `m_accessToken`; maps 401/403 to distinct error codes in the Outcome (add `int status` to
  `Outcome`, or a `VisionResult::status`).
- `src/net/INatParse.{h,cpp}`: `parseVisionResults(QJsonObject)` → `VisionResult{ QList<VisionSuggestion>
  suggestions; std::optional<VisionSuggestion> commonAncestor; }`, `VisionSuggestion{ Taxon taxon;
  double score (combined_score, 0-100); double visionScore; bool seenNearby (frequency_score > 0) }`,
  taxa via the existing `inat::parseTaxon`.

Review pane (`src/ui/ReviewPane.{h,cpp}`):
- Heading row: `chooseHeading` + stretch + `m_suggestButton`; new `QListWidget m_suggestions`
  (hidden until used) between it and `m_candidates`; a status label for errors.
- On click: re-apply `settings().inatApiToken()` to the client (as MainWindow.cpp:3199 does);
  read `latitude, longitude, captured_on` for `currentCaptureId()` with a direct query; render the
  JPEG on a worker (`QtConcurrent`): `ThumbnailCache::render(path, kPreviewPx, raw::extractPreview)`
  → `QImage::save(QBuffer, "JPG", 85)`; then `scoreImage`. Stale-reply guard by capture id
  (like `m_refTaxonInatId`).
- On results: `TaxonomyStore::upsertTaxa` the returned taxa (so confirm writes a real taxon_id —
  `MatchReviewer::writeDecision` stores NULL for an uncached inat id — and the reference photo has a
  URL); tree membership from `inat::LibraryPresence` (`load()` + `inAnyTree`).
- Click on a suggestion: insert a `TaxonCandidate` (matchedVia "iNat suggestion", score) at the top
  of `m_candidates` if absent, select it (existing reference-photo debounce follows).
- Per-session cache `QHash<qint64 captureId, VisionResult>`; cleared/hidden in `showCurrent()`.

Docs: `resources/help/help.html` Review section — what it does, that the photo is sent to
iNaturalist, the token requirement (expires daily), and that it depends on iNaturalist allowing
PhotoLife (with the registration/email steps).

## Critical files
- src/net/Transport.h, src/net/QtNetworkTransport.cpp, src/net/HttpClient.{h,cpp},
  src/net/INatClient.{h,cpp}, src/net/INatParse.{h,cpp}
- src/ui/ReviewPane.{h,cpp} (+ MainWindow only if a "go to iNaturalist Tools" jump is needed)
- tests: tests/tst_httpclient.cpp, tests/tst_inatclient.cpp, tests/tst_inatparse.cpp,
  tests/FakeTransport.h (record verb/body)
- resources/help/help.html

## Verification
- Unit tests: HttpClient POST goes through the queue, carries verb/body/content type and token, is
  not cached; INatClient `scoreImage` sends multipart with image + lat/lng/observed_on (FakeTransport
  inspects body), parses suggestions/common ancestor, and reports 401/403 distinctly; INatParse on a
  recorded sample response.
- Manual, with the user's token, on a few Review Unmatched photos: a 200 shows ranked suggestions;
  clicking one selects it with its reference photo; Confirm writes a match with a real taxon.
  If iNaturalist answers 403 (not yet approved), the message explains and nothing breaks.
- Full `ctest`.
