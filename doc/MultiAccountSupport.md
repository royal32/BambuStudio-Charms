# Multi-Account Support for BambuStudio

This feature allows users to sign into multiple Bambu accounts and easily switch between them without logging out and back in.

## User Guide

### Adding Accounts

1. Open BambuStudio
2. Click the login button on the home page
3. Sign in with your Bambu account
4. Your account is automatically saved
5. To add another account, log out and log in with a different account
6. Both accounts are now saved

### Switching Accounts

1. On the home page, look for the "👤 Accounts" button in the top-right corner
2. Click the button to open the account dropdown
3. You'll see a list of all your saved accounts
4. The active account is marked with "✓ Active" in green
5. Click "Switch" on any inactive account to switch to it
6. Click anywhere else to close the dropdown

### Removing Accounts

1. Open the account dropdown by clicking "👤 Accounts"
2. Click the "×" (remove) button next to the account you want to remove
3. Confirm the removal in the dialog
4. The account will be removed from the list

Note: You must have at least one account, so the remove button is hidden when you only have one account.

## Technical Details

### Account Storage

Accounts are stored in a JSON file located at:
- Windows: `%APPDATA%/BambuStudio/bambu_accounts.json`
- macOS: `~/Library/Application Support/BambuStudio/bambu_accounts.json`
- Linux: `~/.config/BambuStudio/bambu_accounts.json`

The file contains an array of account objects with the following structure:

```json
[
  {
    "user_id": "user123",
    "user_name": "username",
    "nickname": "Display Name",
    "avatar_url": "https://example.com/avatar.jpg",
    "auth_token": "...",
    "is_active": true
  }
]
```

### Security Considerations

- Account authentication tokens are stored locally in the `bambu_accounts.json` file
- The file should be protected with appropriate file system permissions
- Users should be aware that anyone with access to this file could potentially access their accounts
- For enhanced security, consider encrypting the auth_token field in future versions

### Architecture

The multi-account feature consists of several components:

1. **AccountManager** (`src/slic3r/GUI/AccountManager.hpp/cpp`)
   - Manages the list of saved accounts
   - Handles add, remove, and switch operations
   - Persists accounts to/from JSON file

2. **GUI_App Integration** (`src/slic3r/GUI/GUI_App.hpp/cpp`)
   - Initializes AccountManager on startup
   - Saves new accounts when users log in
   - Provides `switch_to_account()` method
   - Handles web API commands for account management

3. **WebViewPanel Integration** (`src/slic3r/GUI/WebViewDialog.hpp/cpp`)
   - Injects account switcher UI into home page
   - Sends account list to JavaScript UI
   - Handles script injection

4. **Account Switcher UI** (`resources/web/account_switcher.js`)
   - Creates floating account switcher button
   - Displays dropdown with account list
   - Provides UI for switching and removing accounts
   - Communicates with C++ via window.postMessage

### Limitations

- The current implementation marks accounts as active but full account switching requires NetworkAgent support for changing authentication tokens
- Account switching will refresh the login info displayed on the home page
- Some operations may still require the user to be fully logged in with the active account

### Future Enhancements

Potential improvements for future versions:

1. **Full NetworkAgent Integration**: Implement proper authentication token switching in NetworkAgent
2. **Encrypted Storage**: Encrypt authentication tokens for better security
3. **Account Sync**: Sync account preferences across devices
4. **Profile Pictures**: Display actual user avatars in the account switcher
5. **Session Management**: Better handling of expired sessions
6. **Keyboard Shortcuts**: Add hotkeys for quick account switching
7. **Account Groups**: Organize accounts into work/personal groups

## Development

### Building with Multi-Account Support

The feature is built automatically when you build BambuStudio normally:

```bash
mkdir build
cd build
cmake ..
make
```

### Testing

Basic tests for AccountManager are provided in `tests/slic3rgui/test_account_manager.cpp`.

To run tests:
```bash
cd build
ctest
```

### Debugging

Enable verbose logging to see account operations:

```bash
./BambuStudio --loglevel=trace
```

Look for log messages containing:
- "AccountManager"
- "switch_to_account"
- "Account switcher script"

## Troubleshooting

### Account Switcher Not Appearing

1. Make sure you have multiple accounts saved
2. Check that JavaScript is enabled in the WebView
3. Look for errors in the log file
4. Verify the account_switcher.js file exists in resources/web/

### Accounts Not Persisting

1. Check file permissions on the bambu_accounts.json file
2. Verify the user data directory is writable
3. Check logs for errors during save/load

### Switch Not Working

1. Ensure the NetworkAgent is initialized
2. Check logs for errors in switch_to_account
3. Verify the account exists in bambu_accounts.json

## Contributing

When contributing to this feature, please:

1. Follow the existing code style
2. Add tests for new functionality
3. Update this README with any new features
4. Test with multiple accounts to ensure proper switching
5. Consider security implications of any changes
