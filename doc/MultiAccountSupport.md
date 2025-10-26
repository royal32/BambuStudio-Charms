# Multi-Account Support for BambuStudio

This feature allows users to sign into multiple Bambu accounts and easily switch between them without logging out and back in.

## ⚠️ Security Notice

**Important**: Account authentication tokens are currently stored in **plaintext** in the `bambu_accounts.json` file. This means:
- Anyone with access to your computer can potentially read your authentication tokens
- Your account tokens are not encrypted at rest
- Protect your computer and user account with strong passwords
- Do not share your `bambu_accounts.json` file with others
- Be aware of this risk when using this feature on shared or public computers

Future versions may include encrypted storage for enhanced security.

## User Guide

### What Works in Current Version

✅ **Available Now**:
- Save multiple Bambu accounts (credentials stored locally)
- View all saved accounts in a dropdown menu
- Mark which account is "active" for display purposes
- Remove accounts from the saved list
- Account data persists across app restarts

⚠️ **Current Limitations**:
- Switching accounts updates the UI but **does not** change the active login session with Bambu's servers
- Cloud operations (printing, presets sync, etc.) still use the originally logged-in account
- Full account switching requires logging out and logging back in with the desired account
- This feature is primarily useful for quickly viewing which accounts you have and managing saved credentials

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

**Note**: Switching accounts in the current version primarily changes which account is marked as active in the UI. For full functionality with a different account, you'll need to log out and log back in.

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

**⚠️ CRITICAL SECURITY WARNING**

Account authentication tokens are stored in **plaintext** in the `bambu_accounts.json` file. This presents the following risks:

1. **Token Exposure**: Anyone with file system access to your user directory can read your authentication tokens
2. **Account Compromise**: If your computer is compromised, attackers could extract tokens and access your Bambu accounts
3. **No Encryption**: Tokens are stored as plain text without any encryption or obfuscation
4. **Shared Computers**: This feature should be used with caution on shared or public computers
5. **Token Expiration**: Stored tokens may expire over time, requiring re-login. The application does not currently notify users when stored tokens become invalid.

**Recommendations**:
- Only use this feature on computers you trust and control
- Use strong passwords to protect your user account
- Regularly change your Bambu account passwords
- If you notice authentication errors, try logging out and back in to refresh the token
- Monitor your account for unauthorized access
- Consider the risks before enabling this feature on laptops that may be lost or stolen
- Be aware that malware on your system could potentially steal these tokens

**Future Improvements**: A future version may implement encrypted storage for authentication tokens to mitigate these risks.

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

### Current Implementation Limitations

**What This Version Does**:
- Saves multiple account credentials locally
- Displays all saved accounts in a dropdown UI
- Allows marking which account is "active" for UI display
- Persists account data across application restarts
- Provides a convenient way to manage saved account credentials

**What This Version Does NOT Do**:
- Does **not** switch the actual authenticated session with Bambu's servers
- Does **not** change which account is used for cloud operations (printing, presets, etc.)
- Does **not** support true "hot-swapping" between accounts without logging out/in

**Technical Reason**: 
The NetworkAgent component (which handles communication with Bambu's servers) currently supports only a single authenticated session at a time. Full account switching would require:
1. NetworkAgent to support switching authentication tokens dynamically
2. Proper session management for multiple concurrent or sequential logins
3. State management for per-account data (presets, machines, etc.)

**Workaround for Users**:
To truly switch accounts, users should:
1. Log out of the current account
2. Log in with the desired account from the saved list
3. The new account will be marked as active

The account switcher serves as a convenient credential manager and reminder of which account is currently active, but does not replace the login/logout flow.

### Future Enhancements

Potential improvements for future versions:

1. **Full NetworkAgent Integration**: Implement proper authentication token switching in NetworkAgent to enable true multi-account sessions
2. **Encrypted Storage**: Encrypt authentication tokens for better security (HIGH PRIORITY)
3. **Account Sync**: Sync account preferences across devices
4. **Profile Pictures**: Display actual user avatars in the account switcher
5. **Session Management**: Better handling of expired sessions and automatic re-authentication
6. **Keyboard Shortcuts**: Add hotkeys for quick account switching (e.g., Ctrl+Shift+1/2/3)
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
