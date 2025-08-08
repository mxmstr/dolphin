// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>

class QComboBox;
class QPushButton;

class LuaWindow : public QDialog
{
	Q_OBJECT
public:
	explicit LuaWindow(QWidget* parent);

private:
	void OnButtonPressed();
	void OnSelectionChanged(const QString& text);
	void PopulateScriptList();

	QComboBox* m_script_choice;
	QPushButton* m_start_button;
	QPushButton* m_stop_button;
};
