#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "ClipSelectionActivity.h"
#include "EpubReaderMenuActivity.h"
#include "LinkNoteTitle.h"
#include "ManualPageTurnQueue.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Epub::SyncMetadata> vocabularyMetadata;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  ManualPageTurnQueue manualPageTurns;
  std::atomic<bool> cancelRenderRequested{false};
  bool pendingManualTurnTouch = false;
  bool pageWasSkimmed = false;
  bool shouldCancelRender() const;
  static bool shouldCancelRenderCallback(void* ctx);
  bool pageTurnBy(int steps);
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool showFontDamagedMessage = false;
  unsigned long fontDamagedMessageTime = 0UL;
  bool showClippingMessage = false;
  StrId clippingMessage = StrId::STR_CLIPPING_SAVED;
  unsigned long clippingMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  int idlePrewarmOffset = 0;
  unsigned long lastRenderCompleteMs = 0;
  int lastRenderedSpine = -1;
  // Speculative next-page pre-render & controller DTM2 pre-load (0 ms render/SPI on forward turn).
  std::optional<uint64_t> preloadedFingerprint;
  bool stageAttempted = false;
  uint32_t bookmarkRevision = 0;
  int preloadedMarginLeft = 0;
  int preloadedMarginTop = 0;
  std::optional<uint32_t> preloadedVisibleOffset;
  std::vector<FootnoteEntry> preloadedFootnotes;
  std::vector<PageLink> preloadedLinks;
  void clearPreloadedNextPage();
  uint64_t stagedPageFingerprint(const ReaderRenderSpec& spec, int page, int top, int left) const;
  void computePageMargins(int& outTop, int& outRight, int& outBottom, int& outLeft) const;
  void stageNextPageIfIdle();
  bool cacheImagesWithFramebufferLoan(const Page& page, int fontId, int left, int top);
  // Debounced SD progress persistence (eliminates 4-op exFAT write stall on every page turn).
  static constexpr unsigned long PROGRESS_SAVE_DEBOUNCE_MS = 1500;
  static constexpr unsigned long PROGRESS_SAVE_MAX_DELAY_MS = 15000;
  bool progressDirty = false;
  unsigned long progressDirtySinceMs = 0;
  unsigned long progressFirstDirtyMs = 0;
  int pendingSaveSpine = -1;
  int pendingSavePage = -1;
  int pendingSavePageCount = -1;
  std::optional<uint32_t> pendingSaveVisibleOffset;
  void markProgressDirty(int spineIndex, int currentPage, int pageCount);
  void flushPendingProgress(bool force = false);
  // Progressive B/W-first image pages: show B/W immediately and run 4-level grayscale
  // upgrade only if the reader pauses on the image for ~250 ms (still abortable per strip).
  bool deferredImageGrayPending = false;
  bool pageGrayOverlayActive = false;
  int deferredImageGraySpine = -1;
  int deferredImageGrayPage = -1;
  int deferredImageGrayTop = 0;
  int deferredImageGrayLeft = 0;
  unsigned long deferredImageGrayReadyAtMs = 0;
  void clearDeferredImageGray();
  bool deferredImageGrayTick();
  static constexpr int GRAYSCALE_STRIP_ROWS = 80;
  std::unique_ptr<uint8_t[]> grayscaleStripScratch;
  size_t grayscaleStripScratchBytes = 0;
  uint8_t* getGrayscaleStripScratch(size_t bytes);
  void releaseGrayscaleStripScratch();
  // Background next-chapter section pre-build on the final 2 pages of a chapter.
  std::unique_ptr<Section> nextChapterSection;
  int nextChapterSpineIndex = -1;
  int nextChapterFailedSpine = -1;
  uint32_t nextChapterSpecSig = 0;
  void clearNextChapterPrebuild();
  bool prebuildNextChapterTick();
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // A background build step lent the framebuffer: it came back white while the panel still shows the
  // page. Until renderBook() redraws, nothing may be painted straight onto it. Set by the loop task,
  // cleared by the render task.
  std::atomic<bool> pageBufferStale{false};
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed.
  bool overlayRefreshPending = false;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;
  EpubReaderMenuActivity::MenuAction selectedMenuAction = EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;
  // The back-stack outlives the reader (sleep, home) in links.bin so Back
  // still returns to where a followed link was tapped.
  void saveLinkStack() const;
  void loadLinkStack();

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool deferPartialBuildUntilNeeded = false;
  bool embeddedStyleDisabledForBook = false;
  ReaderRenderSpec makeRenderSpec(uint16_t width, uint16_t height) const;
  enum class SectionRenderResult { Ready, BuildFailed, RadioBusy };
  SectionRenderResult prepareSectionForRender(const ReaderRenderSpec& spec);

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;
  // Chapter title shown in the status bar, cached per spine item.
  mutable int statusTitleSpine = -1;
  mutable std::string statusTitle;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Requires the render lock; heap admission is checked separately by the build tick.
  bool backgroundBuildWanted() const;
  enum class BuildHeapGate { Ready, Waiting, Suspended };
  BuildHeapGate buildTickHeapGate(Section& building);
  std::optional<unsigned long> buildHeapPausedSinceMs;
  // One bounded build step on the loop task, in a frame whose input did nothing. Lays out about
  // BACKGROUND_BUILD_PAGES_PER_TICK pages (one 1 KB parse step may add more) and hands the loop
  // back as soon as a button is down, so a short press is seen before more pages are built.
  // Returns true if a repaint was requested (caller should skip idle prewarm for this frame).
  bool backgroundBuildTick();
  void idlePrewarmTick();
  bool anyButtonPressed() const;
  // A background build failed in this spine. The error would likely recur, so from then on the
  // spine is built only as far as the reader goes; pages from earlier build steps stay readable.
  int backgroundBuildFailedSpine = -1;
  // Bluetooth page turner: every chapter build stops the radio until the chapter is built and its
  // page is shown. False: the radio still holds or is taking the heap, and the build must not run.
  std::atomic<bool> radioReleasedForBuild{false};
  bool makeRoomForChapterBuild();
  static constexpr uint32_t BUILD_ROOM_WAIT_MS = 5000;
  static constexpr uint32_t BUILD_ROOM_STEP_MS = 50;
  bool inputOverPage() const override { return overlay != Overlay::None || ReaderActivity::inputOverPage(); }
  // "Connecting Bluetooth…" or "Bluetooth failed" in place of the status bar title.
  mutable LinkNoteTitle linkNoteTitle;
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  int currentTocIndex() const;
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openFootnoteSelect(bool reopenMenuOnCancel);
  void openDictionaryWordSelect();
  // Selection to restore when returning from a dictionary lookup.
  ClipSelectionActivity::Selection pendingLookupSelection;
  ClipSelectionActivity::Selection lookupReturnSelection;
  void startClipSelection(int initialX = -1, int initialY = -1,
                          ClipSelectionActivity::Mode mode = ClipSelectionActivity::Mode::Clip);
  int clippingAtPoint(const Page& page, int x, int y) const;
  void openClippings();
  void applyProgressChange(const ProgressChangeResult& progress);
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  bool renderContents(const Page& page, int orientedMarginTop, int orientedMarginLeft, bool skimPage);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  // Modal shown when a protected book refuses to open (loan expired /
  // date unverified); OK exits, "Sync time" (when offered) verifies the
  // clock over Wi-Fi and reopens the book.
  OptionPopup loadFailurePopup;
  // Protection error captured by loadBook() for handleLoadFailure(); the
  // failed Epub itself does not outlive loadBook().
  std::string loadProtectionError;

  bool loadBook() override;
  bool handleLoadFailure() override;
  // Wi-Fi join + SNTP for a loan whose date could not be verified, then a
  // clean re-open of the book.
  void beginLoanTimeSync();
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  int getProgressBasisPoints() const override;
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void loop() override;
  void onExit() override;
  void prepareForSleep() override;
  void ensureFreshFrameBuffer() override;
  void render(RenderLock&& lock) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool remoteShortcut(bool readerMenu) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
