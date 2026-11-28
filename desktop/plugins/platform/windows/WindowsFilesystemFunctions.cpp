/*
 * WindowsFilesystemFunctions.cpp - implementation of WindowsFilesystemFunctions class
 *
 * Copyright (c) 2017-2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Ikeback - https://github.com/zk26/ikeback
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <QDir>
#include <QFileInfo>

#include <fcntl.h>
#include <io.h>

// ikeback: newer MinGW-w64 headers no longer pull in the basic Windows types
// transitively. wtsapi32.h etc. require windows.h to be included FIRST.
#include <windows.h>

#include <shlobj.h>
#include <accctrl.h>
#include <aclapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <wtsapi32.h>

#include "UserGroupsBackendManager.h"
#include "WindowsCoreFunctions.h"
#include "WindowsFilesystemFunctions.h"


static QString windowsConfigPath( const KNOWNFOLDERID folderId )
{
	QString result;

	SmartCoTaskMemPtr<wchar_t> path;
	if (SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, path.put()) == S_OK)
	{
		result = QString::fromWCharArray(path.get());
	}

	return result;
}



QString WindowsFilesystemFunctions::personalAppDataPath() const
{
	return windowsConfigPath( FOLDERID_RoamingAppData ) + QDir::separator() + QStringLiteral("Ikeback");
}



QString WindowsFilesystemFunctions::globalAppDataPath() const
{
	return windowsConfigPath( FOLDERID_ProgramData ) + QDir::separator() + QStringLiteral("Ikeback");
}



QString WindowsFilesystemFunctions::globalTempPath() const
{
	return windowsConfigPath( FOLDERID_Windows ) + QDir::separator() + QStringLiteral("Temp");
}



QString WindowsFilesystemFunctions::fileOwnerGroup( const QString& filePath )
{
	PSID ownerSID = nullptr;
	SmartSecurityDescriptor securityDescriptor;

	const auto secInfoResult = GetNamedSecurityInfo(WindowsCoreFunctions::toConstWCharArray( filePath ),
													SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
													&ownerSID, nullptr, nullptr, nullptr, securityDescriptor.put());
	if( secInfoResult != ERROR_SUCCESS )
	{
		vCritical() << "GetSecurityInfo() failed:" << secInfoResult;
		return {};
	}

	DWORD nameSize = 0;
	DWORD domainSize = 0;
	SID_NAME_USE sidNameUse = SidTypeUnknown;

	LookupAccountSid(nullptr, ownerSID, nullptr, &nameSize, nullptr, &domainSize, &sidNameUse);

	if( nameSize == 0 || domainSize == 0)
	{
		vCritical() << "Failed to retrieve buffer sizes:" << GetLastError();
		return {};
	}

	SmartWCharPtr name{new wchar_t[nameSize]};
	SmartWCharPtr domain{new wchar_t[domainSize]};

	if (LookupAccountSid(nullptr, ownerSID, name.get(), &nameSize, domain.get(), &domainSize, &sidNameUse) == false)
	{
		vCritical() << "LookupAccountSid() (2) failed:" << GetLastError();
		return {};
	}

	const auto owner = QStringLiteral("%1\\%2").arg(QString::fromWCharArray(domain.get()), QString::fromWCharArray(name.get()));

	return owner;
}



bool WindowsFilesystemFunctions::setFileOwnerGroup( const QString& filePath, const QString& ownerGroup )
{
	WindowsCoreFunctions::SecurityIdentifierBuffer ownerGroupSIDBuffer{};
	const auto ownerGroupSID = reinterpret_cast<PSID>(ownerGroupSIDBuffer.data());
	DWORD ownerGroupSIDLen = ownerGroupSIDBuffer.size();
	std::array<wchar_t, DOMAIN_LENGTH> domain{};

	DWORD domainLen = domain.size();
	SID_NAME_USE sidNameUse;

	if( LookupAccountName( nullptr, WindowsCoreFunctions::toConstWCharArray( ownerGroup ),
						   ownerGroupSID, &ownerGroupSIDLen,
						   domain.data(), &domainLen, &sidNameUse ) == false )
	{
		const auto sidString = IkebackCore::userGroupsBackendManager().configuredBackend()->userGroupSecurityIdentifier(ownerGroup);
		if (sidString.isEmpty() ||
			WindowsCoreFunctions::stringToSecurityIdentifier(sidString, ownerGroupSIDBuffer) == false)
		{
			vCritical() << "Could not look up SID structure:" << GetLastError();
			return false;
		}
	}

	WindowsCoreFunctions::enablePrivilege( SE_TAKE_OWNERSHIP_NAME, true );
	WindowsCoreFunctions::enablePrivilege( SE_RESTORE_NAME, true );

	const auto filePathWide = WindowsCoreFunctions::toWCharArray( filePath );

	const auto result = SetNamedSecurityInfo(filePathWide.get(), SE_FILE_OBJECT,
											 OWNER_SECURITY_INFORMATION, ownerGroupSID, nullptr, nullptr, nullptr);

	if( result != ERROR_SUCCESS )
	{
		vCritical() << "SetNamedSecurityInfo() failed:" << result;
	}

	WindowsCoreFunctions::enablePrivilege( SE_TAKE_OWNERSHIP_NAME, false );
	WindowsCoreFunctions::enablePrivilege( SE_RESTORE_NAME, false );

	return result == ERROR_SUCCESS;
}



bool WindowsFilesystemFunctions::setFileOwnerGroupPermissions( const QString& filePath, QFile::Permissions permissions )
{
	PSID ownerSID = nullptr;
	SmartSecurityDescriptor securityDescriptor;

	const auto filePathWide = WindowsCoreFunctions::toWCharArray( filePath );

	const auto secInfoResult = GetNamedSecurityInfo(filePathWide.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
													&ownerSID, nullptr, nullptr, nullptr, securityDescriptor.put());

	if( secInfoResult != ERROR_SUCCESS )
	{
		vCritical() << "GetSecurityInfo() failed:" << secInfoResult;
		return false;
	}

	SmartSID adminSID;
	SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
	if (AllocateAndInitializeSid(&SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
								 0, 0, 0, 0, 0, 0, adminSID.put()) == false)
	{
		return false;
	}

	static constexpr auto ExplicitAccessCount = 2;
	std::array<EXPLICIT_ACCESS, ExplicitAccessCount> ea{};

	// set read access for owner
	ea[0].grfAccessPermissions = 0;
	if( permissions & QFile::ReadOwner )
	{
		ea[0].grfAccessPermissions |= GENERIC_READ;
	}
	if( permissions & QFile::WriteOwner )
	{
		ea[0].grfAccessPermissions |= GENERIC_WRITE;
	}
	if( permissions & QFile::ExeOwner )
	{
		ea[0].grfAccessPermissions |= GENERIC_EXECUTE;
	}
	ea[0].grfAccessMode = SET_ACCESS;
	ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea[0].Trustee.ptstrName = LPTSTR(ownerSID);

	// set full control for Administrators
	ea[1].grfAccessPermissions = GENERIC_ALL;
	ea[1].grfAccessMode = SET_ACCESS;
	ea[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea[1].Trustee.ptstrName = LPTSTR(adminSID.get());

	SmartACL acl;
	if (SetEntriesInAcl( ExplicitAccessCount, ea.data(), nullptr, acl.put()) != ERROR_SUCCESS)
	{
		vCritical() << "SetEntriesInAcl() failed";
		return false;
	}

	const auto result = SetNamedSecurityInfo(filePathWide.get(), SE_FILE_OBJECT,
											 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
											 nullptr, nullptr, acl.get(), nullptr);

	if( result != ERROR_SUCCESS )
	{
		vCritical() << "SetNamedSecurityInfo() failed:" << result;
	}

	return result == ERROR_SUCCESS;
}



bool WindowsFilesystemFunctions::openFileSafely( QFile* file, QIODevice::OpenMode openMode, QFileDevice::Permissions permissions )
{
	if( file == nullptr )
	{
		return false;
	}

	const auto absolutePath = QFileInfo( file->fileName() ).absoluteFilePath();
	const auto parentPath = QFileInfo( absolutePath ).absoluteDir().canonicalPath();
	if( parentPath.isEmpty() )
	{
		return false;
	}
	const auto expectedPath = QDir( parentPath ).filePath( QFileInfo( absolutePath ).fileName() );

	DWORD access = 0;
	if( openMode & QIODevice::ReadOnly ) { access |= GENERIC_READ; }
	if( openMode & QIODevice::WriteOnly ) { access |= GENERIC_WRITE; }
	if( access == 0 ) { access = GENERIC_READ; }

	DWORD creation = OPEN_EXISTING;
	if( openMode & QIODevice::NewOnly ) { creation = CREATE_NEW; }
	else if( openMode & QIODevice::Truncate ) { creation = CREATE_ALWAYS; }
	else if( openMode & QIODevice::WriteOnly ) { creation = OPEN_ALWAYS; }

	const auto nativePath = QDir::toNativeSeparators( absolutePath );
	SmartFileHandle handle( CreateFileW( WindowsCoreFunctions::toConstWCharArray( nativePath ), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr ) );
	if( handle.isInvalid() )
	{
		return false;
	}

	FILE_ATTRIBUTE_TAG_INFORMATION tagInfo{};
	if( GetFileInformationByHandleEx( handle.get(), FileAttributeTagInfo, &tagInfo, sizeof( tagInfo ) ) == false ||
		( ( tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) &&
		  ( tagInfo.ReparseTag == IO_REPARSE_TAG_SYMLINK || tagInfo.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT ) ) )
	{
		return false;
	}

	std::vector<wchar_t> finalPath( 32768 );
	const auto finalPathLength = GetFinalPathNameByHandleW( handle.get(), finalPath.data(), finalPath.size(), FILE_NAME_NORMALIZED );
	if( finalPathLength == 0 || finalPathLength >= finalPath.size() )
	{
		return false;
	}

	auto actualPath = QString::fromWCharArray( finalPath.data(), int( finalPathLength ) );
	if( actualPath.startsWith( QStringLiteral("\\\\?\\UNC\\") ) )
	{
		actualPath = QStringLiteral("\\\\") + actualPath.mid( 8 );
	}
	else if( actualPath.startsWith( QStringLiteral("\\\\?\\") ) )
	{
		actualPath.remove( 0, 4 );
	}
	actualPath = QDir::fromNativeSeparators( actualPath );
	if( actualPath.compare( expectedPath, Qt::CaseInsensitive ) != 0 )
	{
		return false;
	}

	int descriptorFlags = _O_BINARY;
	descriptorFlags |= access & GENERIC_WRITE ? _O_WRONLY : _O_RDONLY;
	if( openMode & QIODevice::Append ) { descriptorFlags |= _O_APPEND; }
	const int descriptor = _open_osfhandle( reinterpret_cast<intptr_t>( handle.get() ), descriptorFlags );
	if( descriptor < 0 )
	{
		return false;
	}

	handle.release();
	if( file->open( descriptor, openMode, QFileDevice::AutoCloseHandle ) == false )
	{
		_close( descriptor );
		return false;
	}
	if( file->setPermissions( permissions ) == false )
	{
		file->close();
		return false;
	}
#ifdef Q_OS_WIN
	if( ( openMode & QIODevice::WriteOnly ) &&
		setFileOwnerGroupPermissions( file->fileName(), permissions ) == false )
	{
		file->close();
		return false;
	}
#endif
	return true;
}


PlatformCoreFunctions::ProcessId WindowsFilesystemFunctions::findFileLockingProcess(const QString& filePath) const
{
	auto filePathLower = QDir::toNativeSeparators(filePath).toLower();
	if (filePathLower.startsWith(QLatin1String("\\\\?\\")))
	{
		filePathLower = filePathLower.mid(4);
	}

	const auto currentProcessId = GetCurrentProcessId();

	static const size_t InfoBufferSize = 4 * 1024 * 1024;
	std::vector<char> handleInfoBuffer(InfoBufferSize);
	ULONG returnLength = 0;
	NTSTATUS status = NtQuerySystemInformation(SystemHandleInformation, handleInfoBuffer.data(),
											   handleInfoBuffer.size(), &returnLength);
	if (status != 0)
	{
		return PlatformCoreFunctions::InvalidProcessId;
	}

	const auto handleInfo = PSYSTEM_HANDLE_INFORMATION(handleInfoBuffer.data());

	for (ULONG i = 0; i < handleInfo->Count; i++)
	{
		const auto& sh = handleInfo->Handle[i];

		if (sh.OwnerPid == currentProcessId)
		{
			continue;
		}

		const SmartHandle processHandle{OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sh.OwnerPid)};
		if (processHandle.isInvalid())
		{
			continue;
		}

		SmartHandle duplicatedHandle;
		if (!DuplicateHandle(processHandle.get(), (HANDLE)(ULONG_PTR)sh.HandleValue,
							 GetCurrentProcess(), duplicatedHandle.put(), 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			continue;
		}

		if (GetFileType(duplicatedHandle.get()) == FILE_TYPE_DISK)
		{
			WCHAR currentFilePath[MAX_PATH * 4] = { 0 };
			const auto result = GetFinalPathNameByHandleW(duplicatedHandle.get(), currentFilePath, MAX_PATH * 4, FILE_NAME_NORMALIZED);
			if (result > 0)
			{
				auto currentFilePathLower = QString::fromWCharArray(currentFilePath).toLower();
				if (currentFilePathLower.startsWith(QLatin1String("\\\\?\\")))
				{
					currentFilePathLower = currentFilePathLower.mid(4);
				}

				if (filePathLower == currentFilePathLower)
				{
					return sh.OwnerPid;
				}
			}
		}
	}

	return PlatformCoreFunctions::InvalidProcessId;
}
