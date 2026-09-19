#pragma once

#include "NCLPDataThread.h"

#include <VisualizerEditorHeaders.h>

#include <cstdint>

namespace nclp
{

class ImpedanceContent;
class NCLPControlPage;
class NCLPImpedanceProgressWindow;
class NCLPRippleBaselinePanel;

enum class NCLPCanvasPage : uint8_t
{
    Impedance,
    Algorithm,
    Output
};

/**
 * Acquisition Board-style channel and impedance page.
 *
 * The viewport, toolbar coordinates, 250 px headstage columns, and 22 px
 * channel rows intentionally mirror the reference Acquisition Board canvas.
 */
class NCLPImpedanceCanvas : public Visualizer,
                            public Button::Listener,
                            public ComboBox::Listener
{
public:
    explicit NCLPImpedanceCanvas(
        std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess);
    ~NCLPImpedanceCanvas() override;

    void paint(Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void refreshState() override;
    void updateSettings() override;
    void refresh() override;
    void beginAnimation() override;
    void endAnimation() override;
    void buttonClicked(Button* button) override;
    void comboBoxChanged(ComboBox* comboBox) override;

    void updateSnapshot(const SessionSnapshot& snapshot, bool refreshOutputSettings = false);
    void setPage(NCLPCanvasPage newPage);
    NCLPCanvasPage page() const noexcept;

private:
    Component* activePageComponent() const noexcept;
    void refreshControls();
    void performOutputCommand(NCLPControlPage* page, Command command, CommandArgs args, bool query);
    void pollOutputStatus();
    bool outputCommandPending_ = false;
    bool outputStatusPending_ = false;
    bool outputReadRequested_ = false;
    uint64_t outputCommandGeneration_ = 0;
    void startImpedanceMeasurement();
    void impedanceProgressFinished(const SessionSnapshot& snapshot,
                                   const String& status,
                                   bool userPressedCancel);
    void saveResults();
    void setNamingScheme(int scheme);

    std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess_;
    SessionSnapshot latestSnapshot_{};
    bool acquisitionActive = false;
    bool suppressNamingCallback_ = false;
    int namingScheme_ = 1;
    NCLPCanvasPage activePage_ = NCLPCanvasPage::Impedance;
    Point<int> impedanceViewPosition_{};

    std::unique_ptr<Label> numberingSchemeLabel;
    std::unique_ptr<ComboBox> numberingScheme;
    std::unique_ptr<UtilityButton> measureButton;
    std::unique_ptr<UtilityButton> saveButton;
    std::unique_ptr<ImpedanceContent> content;
    std::unique_ptr<NCLPControlPage> algorithmContent;
    std::unique_ptr<NCLPControlPage> outputContent;
    std::unique_ptr<NCLPRippleBaselinePanel> rippleBaselinePanel;
    // Declared after every page so it is destroyed first; it holds a
    // non-owning viewed-component pointer to the active page.
    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<FileChooser> saveChooser;
    std::unique_ptr<NCLPImpedanceProgressWindow> impedanceProgressWindow;

    friend class NCLPImpedanceProgressWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NCLPImpedanceCanvas);
};

} // namespace nclp
