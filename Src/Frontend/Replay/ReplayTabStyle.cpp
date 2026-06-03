#include "ReplayTabStyle.h"

namespace bte::frontend {

QString replayTabStyleSheet() {
  return QStringLiteral(R"(
    #replayTab {
      background: #07111b;
      color: #dce9f5;
      font-family: "Segoe UI", Arial;
      font-size: 13px;
    }
    #replayScrollArea {
      background: #07111b;
      border: 0;
    }
    #replayScrollContent {
      background: #07111b;
    }
    #replayTab QGroupBox,
    #replayChartPanel,
    #replayPlaybackBar,
    #replayTradeLogPanel {
      background: #0b1724;
      border: 1px solid #233a52;
      border-radius: 6px;
    }
    #replayTab QGroupBox,
    #replayChartPanel {
      margin-top: 10px;
      padding-top: 12px;
    }
    #replayTab QGroupBox::title {
      color: #f4f8fb;
      subcontrol-origin: margin;
      left: 12px;
      padding: 0 6px;
      font-size: 16px;
      font-weight: 700;
    }
    #replayTab QLabel {
      color: #d4e5f4;
      font-weight: 500;
    }
    #replayTitleLabel {
      color: #f6fbff;
      font-size: 20px;
      font-weight: 800;
    }
    #replaySubtitleLabel {
      color: #89a6bd;
      font-size: 12px;
      font-weight: 600;
    }
    #replayTab QComboBox,
    #replayTab QDateEdit,
    #replayTab QDoubleSpinBox {
      background: #06101a;
      border: 1px solid #2b4663;
      border-radius: 5px;
      color: #f7fbff;
      min-height: 30px;
      padding: 4px 10px;
      selection-background-color: #1464d9;
    }
    #replayTab QPushButton,
    #replayTab QToolButton {
      background: #0d1d2c;
      border: 1px solid #29435e;
      border-radius: 6px;
      color: #dce9f5;
      min-height: 34px;
      padding: 5px 14px;
    }
    #replayTab QToolButton {
      min-width: 64px;
      padding: 5px;
    }
    #replayLoadButton,
    #replayPlayPauseButton {
      background: #1f63c6;
      border: 1px solid #3b7df0;
      color: #ffffff;
      font-weight: 700;
    }
    #replayStepBackButton,
    #replayStepForwardButton,
    #replayZoomOutButton,
    #replayZoomInButton,
    #replayZoomResetButton {
      background: #0a1826;
      border: 1px solid #2a425c;
      color: #cfe0ee;
    }
    #replayZoomResetButton {
      min-width: 58px;
    }
    #replayTab QPushButton:hover,
    #replayTab QToolButton:hover {
      background: #14283b;
      border-color: #3a5877;
    }
    #replayLoadButton:hover,
    #replayPlayPauseButton:hover {
      background: #2874df;
      border-color: #5790ff;
    }
    #replayTab QPushButton:pressed,
    #replayTab QToolButton:pressed {
      background: #0a1420;
    }
    #replayLoadButton:pressed,
    #replayPlayPauseButton:pressed {
      background: #184d9d;
    }
    #replayProgressBar {
      background: #07111b;
      border: 1px solid #29435e;
      border-radius: 5px;
      color: transparent;
      max-height: 14px;
    }
    #replayProgressBar::chunk {
      background: #27b6c7;
      border-radius: 5px;
    }
    #replayVolumePlaceholder {
      background: #07111b;
      border: 1px solid #233a52;
      border-radius: 6px;
      color: #aebfd0;
      font-size: 15px;
      font-weight: 700;
    }
    #replayPortfolioBox QLabel {
      background: #07131f;
      border: 1px solid #1f354d;
      border-radius: 5px;
      color: #dce9f5;
      padding: 8px 10px;
    }
    #replayTradeLogTitle {
      color: #f4f8fb;
      font-size: 15px;
      font-weight: 700;
    }
    #replayTradeLogTable {
      background: #07111b;
      alternate-background-color: #0b1724;
      border: 0;
      color: #dce9f5;
      gridline-color: #233a52;
      selection-background-color: #145fc7;
    }
    #replayTradeLogTable QHeaderView::section {
      background: #111f2f;
      border: 0;
      border-right: 1px solid #263f59;
      color: #dce9f5;
      font-weight: 700;
      padding: 7px;
    }
  )");
}

} // namespace bte::frontend
