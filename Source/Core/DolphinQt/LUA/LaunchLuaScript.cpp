// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/LUA/LaunchLuaScript.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Core/Core.h"
#include "Core/LUA/Lua.h"

LuaWindow::LuaWindow(QWidget* parent) : QDialog(parent)
{
	setWindowTitle(tr("Lua Scripts"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	m_script_choice = new QComboBox();
	m_start_button = new QPushButton(tr("Start"));
	m_stop_button = new QPushButton(tr("Stop"));

	auto* const main_layout = new QVBoxLayout();
	auto* const form_layout = new QFormLayout();
	auto* const button_layout = new QHBoxLayout();

	button_layout->addWidget(m_start_button);
	button_layout->addWidget(m_stop_button);

	form_layout->addRow(tr("Script File:"), m_script_choice);
	form_layout->addRow(button_layout);

	main_layout->addLayout(form_layout);

	setLayout(main_layout);

	connect(m_start_button, &QPushButton::clicked, this, &LuaWindow::OnButtonPressed);
	connect(m_stop_button, &QPushButton::clicked, this, &LuaWindow::OnButtonPressed);
	connect(m_script_choice, &QComboBox::currentTextChanged, this, &LuaWindow::OnSelectionChanged);

	PopulateScriptList();
}

void LuaWindow::PopulateScriptList()
{
	m_script_choice->clear();

	const std::string scripts_path = File::GetUserPath(D_SCRIPTS_IDX);
	const std::vector<std::string> scripts = File::ScanDirectoryTree(scripts_path, ".lua", true);

	for (const auto& script : scripts)
	{
		std::string P, F, E;
		File::SplitPath(script, &P, &F, &E);
		if (F.substr(0, 1) != "_")
			m_script_choice->addItem(QString::fromStdString(F + E));
	}
}

void LuaWindow::OnButtonPressed()
{
	if (!Core::IsRunningAndStarted())
	{
		return;
	}

	const std::string script_name = m_script_choice->currentText().toStdString();

	if (sender() == m_start_button)
	{
		if (Lua::IsScriptRunning(script_name))
			return;

		Lua::LoadScript(script_name);
	}
	else if (sender() == m_stop_button)
	{
		if (!Lua::IsScriptRunning(script_name))
			return;

		Lua::TerminateScript(script_name);
	}
	OnSelectionChanged(m_script_choice->currentText());
}

void LuaWindow::OnSelectionChanged(const QString& text)
{
	if (text.isEmpty())
	{
		m_start_button->setEnabled(false);
		m_stop_button->setEnabled(false);
		return;
	}

	const bool is_running = Lua::IsScriptRunning(text.toStdString());
	m_start_button->setEnabled(!is_running);
	m_stop_button->setEnabled(is_running);
}
