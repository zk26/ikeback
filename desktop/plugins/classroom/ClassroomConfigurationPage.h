/*
 * ClassroomConfigurationPage.h - Ikeback Configurator page for the classroom extension
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include "ConfigurationPage.h"

class QCheckBox;
class QLineEdit;
class QSpinBox;
class ClassroomConfiguration;

class ClassroomConfigurationPage : public ConfigurationPage
{
	Q_OBJECT
public:
	explicit ClassroomConfigurationPage( ClassroomConfiguration& configuration, QWidget* parent = nullptr );

	void resetWidgets() override;
	void connectWidgetsToProperties() override;
	void applyConfiguration() override {}

private:
	ClassroomConfiguration& m_configuration;

	QCheckBox* m_enabled = nullptr;
	QLineEdit* m_dataDirectory = nullptr;
	QCheckBox* m_exitProtection = nullptr;
	QLineEdit* m_exitAdminUsername = nullptr;
	QCheckBox* m_allowSelfLogin = nullptr;
	QCheckBox* m_aiEnabled = nullptr;
	QLineEdit* m_aiEndpoint = nullptr;
	QLineEdit* m_aiApiKey = nullptr;
	QLineEdit* m_aiModel = nullptr;
	QLineEdit* m_aiSystemPrompt = nullptr;
	QSpinBox* m_aiTimeout = nullptr;
	QCheckBox* m_monitoringEnabled = nullptr;
	QSpinBox* m_captureInterval = nullptr;
	QSpinBox* m_retentionDays = nullptr;
	QCheckBox* m_syncEnabled = nullptr;
	QLineEdit* m_silencePageText = nullptr;
	QLineEdit* m_wordCloudStopWords = nullptr;
};
