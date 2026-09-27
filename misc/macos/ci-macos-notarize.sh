#!/bin/sh

set -e

# Notarizes each .dmg or .zip given, and staples the ticket to each .dmg
# (a zip can't hold one: staple the app inside and zip it again). The
# credentials are an App Store Connect API key, the .p8 file's contents in
# APPLE_NOTARY_KEY, with APPLE_NOTARY_KEY_ID and APPLE_NOTARY_ISSUER_ID,
# or an Apple ID: APPLE_NOTARIZATION_USERNAME, APPLE_NOTARIZATION_PASSWORD
# (an app-specific password) and APPLE_TEAM_ID.

if [ -n "${APPLE_NOTARY_KEY}" ]
then
    KEY_FILE=$(mktemp)
    trap 'rm -f "${KEY_FILE}"' EXIT
    printf '%s\n' "${APPLE_NOTARY_KEY}" > "${KEY_FILE}"
elif [ -n "${APPLE_NOTARIZATION_USERNAME}" ]
then
    echo "Creating NotarizationProfile..."
    xcrun notarytool store-credentials --apple-id "${APPLE_NOTARIZATION_USERNAME}" \
        --password "${APPLE_NOTARIZATION_PASSWORD}" \
        --team-id "${APPLE_TEAM_ID}" "NotarizationProfile"
else
    echo "No notarization credentials supplied, skipping..."
    exit 0
fi

# notarytool with the credentials
notarytool()
{
    if [ -n "${KEY_FILE}" ]
    then
        xcrun notarytool "$@" --key "${KEY_FILE}" \
            --key-id "${APPLE_NOTARY_KEY_ID}" --issuer "${APPLE_NOTARY_ISSUER_ID}"
    else
        xcrun notarytool "$@" --keychain-profile "NotarizationProfile"
    fi
}

if [ "$#" -eq 0 ]
then
    echo "Error: Please provide one or more .dmg or .zip files"
    exit 1
fi

for FILE in "$@"; do
    case ${FILE} in
        *.dmg|*.zip)
            if [ ! -f "${FILE}" ]
            then
                echo "Error: '${FILE}' does not exist or is not a regular file"
                exit 1
            fi

            echo "Submitting notarization request..."
            RESULT=$(notarytool submit "${FILE}" --wait --output-format plist) || true
            STATUS=$(echo "${RESULT}" | plutil -extract status raw -o - - 2>/dev/null) || true
            if [ "${STATUS}" != "Accepted" ]
            then
                echo "${RESULT}"
                ID=$(echo "${RESULT}" | plutil -extract id raw -o - - 2>/dev/null) || true
                if [ -n "${ID}" ]
                then
                    notarytool log "${ID}" || true
                fi
                echo "Error: notarization of '${FILE}' failed: ${STATUS:-no status}"
                exit 1
            fi

            case ${FILE} in
                *.dmg)
                    echo "Stapling..."
                    xcrun stapler staple "${FILE}"
                    ;;
            esac
            ;;

        *)
            echo "Error: '${FILE}' does not have a .dmg or .zip extension"
            exit 1
            ;;
    esac
done
