/*
 * ClassroomSignInDialog.h - ikeback name + seat sign-in dialog
 *
 * Replaces Ikeback's native username/password logon (usersessioncontrol) with
 * the classroom identity model: a student name and a seat number. No Windows
 * credentials are collected, stored or transmitted.
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>
#include <QStringList>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;

class ClassroomSignInDialog : public QDialog
{
	Q_OBJECT
public:
	explicit ClassroomSignInDialog( const QStringList& classNames,
									const QString& currentClassName,
									const QJsonArray& classes,
									QWidget* parent = nullptr );

	QString className() const;
	QString studentName() const;
	QString seat() const;

private:
	void updateOkButton();
	void refreshRosterStudents();
	void selectRosterStudent( int index );

	const QJsonArray m_classes;
	QComboBox* m_classCombo = nullptr;
	QComboBox* m_studentCombo = nullptr;
	QLineEdit* m_nameEdit = nullptr;
	QLineEdit* m_seatEdit = nullptr;
	QDialogButtonBox* m_buttons = nullptr;
};
