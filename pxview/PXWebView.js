import React, { Component } from 'react';
import { View } from 'react-native';
import WebView from 'react-native-webview';
import ProgressBar from 'react-native-progress/Bar';
import { withTheme } from 'react-native-paper';
import { globalStyles } from '../styles';

class PXWebView extends Component {
  constructor(props) {
    super(props);
    this.state = {
      loading: false,
    };
  }

  handleOnLoadStart = () => {
    this.setState({
      loading: true,
    });
  };

  handleOnLoadEnd = () => {
    this.setState({
      loading: false,
    });
  };

  css_rule = ['iframe', '.d_header', '#share-buttons', '.actions + section[style^="text-align: center; background: #FFF"]', '#footer_ad', '#back-to-top:not(html):not(body)', '#dynamic-biron', 'a[href^="https://twitter.com/intent/tweet?"]', 'a[href*="https://www.facebook.com/sharer.php"]'].map(s=>s+'{display: block;}').join(' ')

  render() {
    const { source, theme, ...otherProps } = this.props;
    const { loading } = this.state;
    return (
      <View
        style={[
          globalStyles.container,
          { backgroundColor: theme.colors.background },
        ]}
      >
        {loading && (
          <ProgressBar
            indeterminate
            borderRadius={0}
            width={null}
            useNativeDriver
          />
        )}
        <WebView
          source={source}
          onLoadStart={this.handleOnLoadStart}
          onLoadEnd={this.handleOnLoadEnd}
          injectedJavaScript={`
            const style = document.createElement('style');
            style.textContent = '${this.css_rule}';
            document.documentElement.appendChild(style);
          `}
          onMessage={()=>{}}
          startInLoadingState
          {...otherProps}
        />
      </View>
    );
  }
}

export default withTheme(PXWebView);
